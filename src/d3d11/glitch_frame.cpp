#include "glitch_frame.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"

namespace edvr {
namespace {

// Frames of camera history held in memory and written out only when asked.
//
// About ten seconds at 90Hz. Nothing is written to disk unless the dump key is
// pressed, so the cost of keeping it is one 60-byte struct per frame.
constexpr uint32_t kRingFrames = 900;

// Frames to stand down for after deciding a jump was a change of reference
// frame rather than a glitch.
constexpr uint32_t kRebaseCooldown = 120;

// The detector must never be able to withhold frames continuously. If more than
// this fraction of a window is being withheld, whatever it is watching is not
// the thing it was built for, and it switches itself off for the session.
constexpr uint32_t kRunawayWindow = 2000;
constexpr uint32_t kRunawayLimit = 40;      // 2% of the window

// RENDERED frames to wait for the camera buffer before concluding it is not
// there on this build.
//
// Rendered, not presented, and the distinction is the whole point. Counting
// presents, this gave up after 5001 frames in 2.8 seconds -- the main menu and
// loading screen present at about 1800fps and draw no scene, so the buffer
// legitimately never appears, and the fix switched itself off before the game
// had drawn a single frame of a world. Roughly half a minute of actual play.
constexpr uint32_t kNoBufferGiveUp = 2000;

// Frames of a rendered scene needed before the detector is allowed to act.
constexpr uint32_t kValidateFrames = 300;

// Rendered frames after which a value that still has not moved is accepted as
// not being the camera.
//
// Deliberately large -- several minutes of actual play. The 300-frame window
// answers "is it moving NOW", which a docked or landed player makes false
// without saying anything about the offset; this answers "has it EVER moved",
// which only a wrong offset makes false.
constexpr uint32_t kValidateGiveUp = 20000;

struct RingEntry {
    uint64_t qpc;
    uint32_t frame;
    uint32_t eyeDraws;
    float    pos[3];
};

struct State {
    bool     enabled = false;
    bool     disabledForSession = false;

    // Which buffer holds the camera, and where in it.
    //
    // Found by size. This is the one part of this fix that is specific to a
    // build of the game rather than to the shape of what it does -- there is no
    // instruction pattern to recognise here, only a block of floats. If the
    // buffer is absent, or what is at that offset does not behave like a camera,
    // the fix disables itself rather than guessing (see validate() below).
    uint32_t bufferBytes = 5376;
    uint32_t posOffset = 1100;     // in floats

    float    jumpMin = 2000.0f;    // absolute floor, world units
    float    jumpFactor = 8.0f;    // or this multiple of the current speed
    uint32_t maxConsecutive = 2;
    uint32_t minEyeDraws = 100;

    // Prediction, built from the previous two frames.
    float    camPrev[3] = {};
    float    camPrev2[3] = {};
    uint32_t camPrevValid = 0;

    // The path the camera left, kept so the next frame can be asked whether it
    // came back to it.
    float    preJumpPrev[3] = {};
    float    preJumpPrev2[3] = {};
    bool     awaitingReturn = false;

    // This frame's furthest-from-origin camera.
    float    frameFarPos[3] = {};
    float    frameFarMag2 = -1.0f;

    bool     jumpedThisFrame = false;
    bool     markedThisFrame = false;
    float    lastResid = 0.0f, lastTrip = 0.0f;

    uint32_t cooldown = 0;
    uint32_t consecutive = 0;

    uint32_t frameNo = 0;
    uint32_t renderedFrames = 0;   // frames that actually drew a scene
    // Eye draws of the PREVIOUS frame. The previous one because this frame's
    // count is still being accumulated while the detector runs, and because the
    // prediction is built from the previous frame's camera anyway.
    uint32_t lastEyeDraws = 0;
    uint32_t framesWithheld = 0;
    uint32_t notesLeft = 40;
    bool     rebaseNoted = false;

    // Runaway guard, counted over a sliding window.
    uint32_t windowFrames = 0;
    uint32_t windowWithheld = 0;

    // Startup validation.
    bool     validated = false;
    bool     sawBuffer = false;

    // Distinct buffers of the configured size, and how each one behaved.
    //
    // The camera buffer is identified by SIZE ALONE, and a size is not unique. If
    // the game has more than one 5376-byte constant buffer, whichever is unmapped
    // gets observed, and a static one can mask the real camera entirely. That is
    // consistent with what a failing session looks like -- a value that moves in
    // 23 of 300 rendered frames rather than 0 or 300 -- but nothing in the log
    // could tell that apart from "the offset is wrong", so it is recorded now.
    struct Candidate {
        const void* res = nullptr;
        uint32_t    seen = 0;
        uint32_t    moved = 0;
        float       last[3] = {};
        float       maxMag2 = 0.0f;
    };
    Candidate candidates[4];
    uint32_t  candidateCount = 0;
    uint32_t  candidatesMissed = 0;   // distinct buffers past the four we track
    uint32_t validateFrames = 0;
    uint32_t validateMoved = 0;
    uint32_t revalidations = 0;

    RingEntry ring[kRingFrames] = {};
    uint64_t  ringHead = 0;
};

// Reads of the game's mapped memory happen inside the caller's fault guard in
// vscreen.cpp, so this file needs no budget of its own.
State* g_state = nullptr;

bool finite3(const float* p) {
    for (uint32_t a = 0; a < 3; ++a) {
        if (!std::isfinite(p[a])) return false;
    }
    return true;
}

// Does what we are watching actually behave like a camera?
//
// The offset above is a measurement from one build, and a future build could put
// anything there. A camera has a property that arbitrary data does not: it moves
// smoothly, so a straight-line guess from the last two frames is usually close.
// Over a few hundred frames of real play the frame-to-frame error against that
// guess sat around 27 units with a 99th percentile of 60, on positions of
// magnitude ten thousand.
//
// So: require the value to move at all -- a constant is not a camera and would
// make the detector silently useless -- and require it not to be constantly
// tripping the threshold, which is what the runaway guard below enforces for the
// rest of the session. Both failures disable the fix and say so.
void validate() {
    State* s = g_state;
    if (s->validated || s->validateFrames < kValidateFrames) return;

    if (s->validateMoved < 30) {
        // A parked player is not a wrong offset.
        //
        // This used to conclude, permanently, from the FIRST 300 rendered
        // frames. Those are whatever the session opens with -- and a session
        // that opens docked, or on a landing pad, has a near-field scene where
        // the furthest object offset is the same value frame after frame.
        // Measured exactly that: 25 of 300 frames, one buffer, 22,950 writes of
        // which 10,529 moved, furthest 125 units. The value was moving; the
        // SCENE was not going anywhere. In the session where this worked the
        // same offset read in the thousands, because the player was flying.
        //
        // So: try again rather than give up. A wrong offset stays wrong however
        // long you wait, which is what the much larger budget below is for; a
        // camera starts moving the moment the player does.
        if (s->renderedFrames < kValidateGiveUp) {
            if (s->revalidations < 3) {
                Log::get().note(
                    "transition flash: the value at float %u of the %u-byte buffer moved "
                    "in only %u of %u frames of rendered scene, furthest %.0f units. That "
                    "is what a stationary scene looks like as well as what a wrong offset "
                    "looks like, so this is not a verdict yet -- watching another %u "
                    "frames. Fly somewhere.",
                    s->posOffset, s->bufferBytes, s->validateMoved, s->validateFrames,
                    static_cast<double>(std::sqrt(s->candidateCount ? s->candidates[0].maxMag2
                                                                   : 0.0f)),
                    kValidateFrames);
            }
            ++s->revalidations;
            s->validateFrames = 0;
            s->validateMoved = 0;
            for (uint32_t i = 0; i < s->candidateCount; ++i) {
                s->candidates[i].seen = 0;
                s->candidates[i].moved = 0;
                s->candidates[i].maxMag2 = 0.0f;
            }
            return;
        }

        s->validated = true;
        s->disabledForSession = true;
        Log::get().note(
            "transition flash fix DISABLED: the %u-byte buffer was found, but the value "
            "at float %u moved in only %u of %u frames of RENDERED SCENE. A camera moves; "
            "this does not, so it is not the camera on this build of the game. Nothing "
            "has been changed and the game renders normally.",
            s->bufferBytes, s->posOffset, s->validateMoved, s->validateFrames);
        // Which buffers of that size were seen, and how each behaved.
        //
        // The size is not unique, so "the value does not move" has two very
        // different causes: the offset is wrong, or a second buffer of the same
        // size is being watched instead of the camera. These lines separate
        // them. A candidate with a large maximum magnitude that moves most
        // frames IS the camera and something else was masking it.
        Log::get().note("  %u distinct %u-byte buffer(s) seen%s:", s->candidateCount,
                        s->bufferBytes,
                        s->candidatesMissed ? " (and more than four existed)" : "");
        for (uint32_t i = 0; i < s->candidateCount; ++i) {
            const State::Candidate& c = s->candidates[i];
            Log::get().note("    buffer %p: moved in %u of %u writes, furthest %.0f units, "
                            "last (%+.2f %+.2f %+.2f)",
                            c.res, c.moved, c.seen,
                            static_cast<double>(std::sqrt(c.maxMag2)),
                            c.last[0], c.last[1], c.last[2]);
        }
        return;
    }
    s->validated = true;
    // By now the compositor hook has long since seen its first eight Submit
    // calls, so this is a reliable moment to say whether the other half is
    // there -- and the most visible line in the log to say it on.
    Log::get().note(
        "transition flash fix ACTIVE: watching the camera at float %u of the %u-byte "
        "scene buffer, which moved in %u of the first %u frames of rendered scene. "
        "Threshold %.0f world units, or %.1fx the current speed, whichever is larger. %s",
        s->posOffset, s->bufferBytes, s->validateMoved, s->validateFrames,
        s->jumpMin, s->jumpFactor,
        glitchConsumerPresent()
            ? "openvr_api.dll is installed, so bad frames will be withheld."
            : "WARNING: openvr_api.dll is NOT installed, so bad frames will be "
              "detected and logged but NOT withheld -- you will still see the flash. "
              "See the openvr folder in the download.");
}

}  // namespace

void installGlitchFrameFix() {
    static State s;
    g_state = &s;

    Config& cfg = Config::get();
    // The switch is a choice and lives in [fix]; the three numbers below it are
    // tuning and live in [advanced], which is where edvr.ini documents them.
    //
    // They were read from [fix] until now, so nothing set them: the section is
    // part of the key, an unknown key is ignored, and a missing one falls back
    // to the default. The shipped ini happens to state the defaults, so this was
    // invisible unless somebody actually changed a value -- at which point the
    // sensitivity they were tuning silently did not move.
    s.enabled = cfg.getBool("fix.transition_flash", true);
    s.jumpMin = cfg.getFloat("advanced.transition_flash_units", 2000.0f);
    s.jumpFactor = cfg.getFloat("advanced.transition_flash_speed_factor", 8.0f);
    // 0 would mean "never withhold anything" -- `consecutive < 0` is never true
    // -- while the log went on saying the fix was armed and then ACTIVE. That is
    // a switch disguised as a limit, and an accidental one: the value is
    // documented as a cap on a run, so 0 reads as "no runs allowed", not as
    // "disable the feature". Anyone who wants it off has fix.transition_flash.
    //
    // The cap is also unreachable above 1 in practice: a marked frame resets the
    // camera history, so the next frame cannot be evaluated and a run of two
    // never forms. The knob is kept because it bounds a future detector that
    // could produce runs, and because removing a documented setting is worse
    // than one that is quietly generous.
    const int maxConsec = cfg.getInt("advanced.transition_flash_max_consecutive", 2);
    if (maxConsec < 1) {
        s.maxConsecutive = 1;
        Log::get().note(
            "transition_flash_max_consecutive = %d would stop every frame from being "
            "withheld while the fix still reported itself active, so 1 is being used. "
            "To turn the fix off, set transition_flash = 0 under [fix].",
            maxConsec);
    } else {
        s.maxConsecutive = static_cast<uint32_t>(maxConsec);
    }
    s.bufferBytes = static_cast<uint32_t>(cfg.getInt("advanced.camera_buffer_bytes", 5376));
    s.posOffset = static_cast<uint32_t>(cfg.getInt("advanced.camera_buffer_offset", 1100));

    if (!s.enabled) {
        Log::get().note("transition flash fix off (fix.transition_flash = 0)");
        return;
    }
    if (s.jumpMin <= 0.0f || s.bufferBytes == 0) {
        s.enabled = false;
        Log::get().note("transition flash fix off: threshold or buffer size is zero.");
        return;
    }
    // Three floats have to fit at that offset, inside that buffer.
    //
    // Checked here and not only at the read, so an impossible pair is refused
    // rather than printed. Echoing a value is what makes a wrong one visible --
    // but echoing one without checking it just states the absurdity and arms
    // anyway: "looking for the camera at float 4294967295 of a 256-byte buffer".
    if (static_cast<uint64_t>(s.posOffset) * 4u + 12u > s.bufferBytes) {
        s.enabled = false;
        Log::get().note(
            "transition flash fix off: camera_buffer_offset %u needs three floats inside "
            "a %u-byte buffer, which does not fit. Check [advanced] in edvr.ini; the "
            "defaults are 1100 and 5376.",
            s.posOffset, s.bufferBytes);
        return;
    }
    // Said at install, not only when validation finishes, so a log from a
    // session that never reached a rendered scene still shows whether this was
    // armed at all. A fix that is silent until it succeeds is indistinguishable
    // from one that was never built in.
    Log::get().note(
        "transition flash fix armed: looking for the camera at float %u of a %u-byte "
        "constant buffer, threshold %.0f units or %.1fx speed, at most %u frames in a "
        "row. It watches the first 300 rendered frames before acting, and reports here "
        "either way. Needs openvr_api.dll installed as well to be able to withhold "
        "anything.",
        s.posOffset, s.bufferBytes, s.jumpMin, s.jumpFactor, s.maxConsecutive);

    // The settings are printed rather than assumed. Three of them were read from
    // the wrong section for the whole of 0.5.x and no log line would have shown
    // it, because the values the ini stated were also the defaults. A number
    // that is never echoed back cannot be told apart from one that is ignored.
}

bool glitchFrameNeedsEyeDraws() {
    State* s = g_state;
    // disabledForSession, like its sibling below. Without it this answers "yes"
    // forever: `enabled` is set once at install and never falls, so once the
    // detector has given up -- no buffer, validation failed, runaway guard --
    // vScreen would go on resolving a view per render-target rebind, every
    // frame, for a consumer that will never look at the count again.
    return s && s->enabled && !s->disabledForSession;
}

bool glitchFrameWantsBuffer(uint32_t bytes) {
    State* s = g_state;
    // Still true after the fix disables itself: the camera history is what a
    // user reports a flash with, and it cannot be recorded if nothing is
    // observed. The cost is one size compare per Map plus the read itself.
    return s && s->enabled && bytes == s->bufferBytes;
}

void glitchFrameObserve(const void* data, uint32_t bytes, const void* resource) {
    State* s = g_state;
    // NOT gated on disabledForSession here. A stood-down fix still tracks the
    // camera so the history dump describes the moment the user pressed the key,
    // rather than the moment the fix gave up. The gate that matters -- never
    // acting -- is below, after the tracking.
    if (!s || !s->enabled) return;
    if (bytes != s->bufferBytes) return;
    // Widened deliberately. posOffset comes from the ini through getInt and is
    // stored unsigned, so a negative or very large value becomes something near
    // 0xFFFFFFFF; (posOffset + 3) * 4 then wraps to a tiny number, this check
    // passes, and the read below lands gigabytes past the buffer. The identical
    // wrap in vscreen.cpp was a hard crash on a documented, hand-edited setting,
    // and this one was left behind when that was fixed.
    if (static_cast<uint64_t>(s->posOffset) * 4u + 12u > bytes) return;

    const float* pos = &static_cast<const float*>(data)[s->posOffset];
    // Non-finite values compare false in both directions, so a NaN here would
    // pass every threshold test silently rather than failing one.
    if (!finite3(pos)) return;

    s->sawBuffer = true;

    // Record which buffer this was, while validation is still deciding.
    if (!s->validated && resource) {
        State::Candidate* c = nullptr;
        for (uint32_t i = 0; i < s->candidateCount; ++i) {
            if (s->candidates[i].res == resource) { c = &s->candidates[i]; break; }
        }
        if (!c && s->candidateCount < 4) c = &s->candidates[s->candidateCount++];
        if (!c) {
            ++s->candidatesMissed;
        } else {
            if (c->res != resource) {
                c->res = resource;
                for (uint32_t a = 0; a < 3; ++a) c->last[a] = pos[a];
            } else if (pos[0] != c->last[0] || pos[1] != c->last[1] ||
                       pos[2] != c->last[2]) {
                ++c->moved;
                for (uint32_t a = 0; a < 3; ++a) c->last[a] = pos[a];
            }
            ++c->seen;
            const float mm = pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2];
            if (mm > c->maxMag2) c->maxMag2 = mm;
        }
    }

    const float m2 = pos[0] * pos[0] + pos[1] * pos[1] + pos[2] * pos[2];
    if (m2 <= s->frameFarMag2) return;
    s->frameFarMag2 = m2;
    for (uint32_t a = 0; a < 3; ++a) s->frameFarPos[a] = pos[a];

    // Everything above is observation. Everything below can withhold a frame,
    // so a fix that has stood down stops exactly here.
    if (s->disabledForSession) return;

    // Re-decide here, on every new furthest camera, rather than at the frame
    // boundary.
    //
    // The decision cannot wait. The real call order is Submit, Submit, Present,
    // WaitGetPoses -- so by the time Present runs, the frame has already gone to
    // the headset. Deciding there withheld nothing at all while the log looked
    // healthy.
    //
    // Evaluating on each new maximum is self-correcting: an early write may set
    // the mark and a later, further one may withdraw it, and whatever stands
    // when Submit arrives is the verdict.
    if (!s->validated || s->camPrevValid < 2) return;

    // Only while a scene is actually being rendered, and the test belongs HERE
    // rather than at the frame boundary.
    //
    // The boundary runs at Present, which is after Submit -- so a mark made here
    // has already been read and acted on by the time the boundary could refuse
    // it. Gating only there withheld 40 frames in a session while logging none
    // of them, because the boundary quietly withdrew a flag the compositor had
    // already seen. Those frames also escaped the consecutive-run cap and the
    // runaway guard, which are the two things that stop this becoming judder.
    //
    // In the galaxy map the camera steps between two fixed points and stays at
    // each, at 43 and 74 eye draws against about 285 in flight, and both states
    // are identical frame to frame. Menus and loading screens are the same story
    // at around 32. None of that is a frame drawn from the wrong place.
    if (s->lastEyeDraws < s->minEyeDraws) return;

    float resid = 0.0f, speed = 0.0f;
    for (uint32_t a = 0; a < 3; ++a) {
        const float pred = s->camPrev[a] + (s->camPrev[a] - s->camPrev2[a]);
        const float e = pos[a] - pred;
        resid += e * e;
        const float v = s->camPrev[a] - s->camPrev2[a];
        speed += v * v;
    }
    resid = sqrtf(resid);
    speed = sqrtf(speed);

    // Compared against a PREDICTION, not against the last position, so
    // travelling fast is not itself suspicious. The speed term then covers
    // acceleration: during a jump the camera legitimately gains thousands of
    // units per frame and a fixed floor alone would fire on all of it.
    const float trip = s->jumpMin > speed * s->jumpFactor ? s->jumpMin
                                                          : speed * s->jumpFactor;
    s->lastResid = resid;
    s->lastTrip = trip;

    const bool jumped = resid > trip;
    if (jumped == s->jumpedThisFrame) return;
    s->jumpedThisFrame = jumped;

    if (jumped && s->cooldown == 0 && s->consecutive < s->maxConsecutive) {
        markGlitchFrame();
    } else if (!jumped) {
        // Withdraw, do not clear: a further candidate later in the same frame
        // can re-raise this.
        unmarkGlitchFrame();
    }
}

void glitchFrameBoundary(uint32_t eyeDraws) {
    State* s = g_state;
    if (!s || !s->enabled) return;

    ++s->frameNo;

    // The gate the detector ACTUALLY used while the frame now ending was being
    // drawn, captured before it is overwritten below. The bookkeeping has to
    // agree with the decision that was made, not re-derive its own from a
    // different frame's draw count -- otherwise a frame can be withheld and not
    // counted, which is the state this whole gate exists to prevent.
    const uint32_t gateDraws = s->lastEyeDraws;
    const bool rendering = gateDraws >= s->minEyeDraws;

    // Published for the detector, which runs during the NEXT frame's rendering
    // and needs to know whether a scene is being drawn. See the note at that
    // test in glitchFrameObserve for why it cannot live here.
    s->lastEyeDraws = eyeDraws;
    if (eyeDraws >= s->minEyeDraws) ++s->renderedFrames;

    // Startup validation, before anything can act on what it sees.
    if (!s->validated) {
        // Only frames that actually RENDERED A SCENE count.
        //
        // Without this, validation runs over whatever 300 frames happen to come
        // first -- and in a menu the camera is legitimately parked, so it
        // concludes the value never moves and disables the fix for the session.
        // Measured doing exactly that: 300 frames at about 100fps with 0 to 22
        // eye draws, then "did not move in 300 frames", four seconds after a
        // 1790fps loading screen and before the game had drawn anything.
        //
        // Both the other users of this counter already had the rule. The
        // give-up branch below counts renderedFrames, for the same reason
        // spelled out in its own comment; the detector itself refuses to act
        // below this threshold in glitchFrameObserve. Validation was the one
        // place still counting frames the game had not drawn.
        //
        // The cost is that validation waits for real rendering, which is what it
        // was always meant to measure. It is a race that used to be won more
        // often than lost, which is worse than losing it consistently: the fix
        // worked in one session and silently did not in the next.
        const bool renderedThisFrame = eyeDraws >= s->minEyeDraws;
        if (s->sawBuffer && s->frameFarMag2 >= 0.0f && renderedThisFrame) {
            ++s->validateFrames;
            if (s->camPrevValid >= 1) {
                float d = 0.0f;
                for (uint32_t a = 0; a < 3; ++a) {
                    const float e = s->frameFarPos[a] - s->camPrev[a];
                    d += e * e;
                }
                if (d > 0.0f) ++s->validateMoved;
            }
        } else if (s->renderedFrames > kNoBufferGiveUp && !s->sawBuffer) {
            // Counted in RENDERED frames. Counted in presented frames this gave
            // up during the loading screen, which presents at about 1800fps and
            // draws no scene, so the buffer had not had a chance to appear.
            s->disabledForSession = true;
            s->validated = true;
            Log::get().note(
                "transition flash fix DISABLED: no %u-byte constant buffer appeared in "
                "%u frames of rendered scene, so the camera cannot be found on this "
                "build. Nothing has been changed and the game renders normally.",
                s->bufferBytes, s->renderedFrames);
        }
        validate();
    }

    if (s->disabledForSession) {
        // Keep recording the history even though nothing will act on it.
        //
        // The dump is the only tool a user has for reporting a flash, and it
        // froze the instant the fix stood down -- so pressing the key after a
        // flash produced the ten seconds before the fix gave up, minutes
        // earlier, with nothing saying so. That is worse than an empty dump: the
        // frames looked plausible and described a completely different moment.
        if (s->frameFarMag2 >= 0.0f) {
            RingEntry& e = s->ring[s->ringHead % kRingFrames];
            e.qpc = static_cast<uint64_t>(qpcNow());
            e.frame = s->frameNo;
            e.eyeDraws = eyeDraws;
            for (uint32_t a = 0; a < 3; ++a) e.pos[a] = s->frameFarPos[a];
            ++s->ringHead;
        }
        s->frameFarMag2 = -1.0f;
        s->jumpedThisFrame = false;
        return;
    }

    if (s->jumpedThisFrame && rendering && s->cooldown == 0 &&
        s->consecutive < s->maxConsecutive) {
        s->markedThisFrame = true;
        ++s->framesWithheld;
        ++s->windowWithheld;
        if (s->notesLeft > 0) {
            --s->notesLeft;
            const bool acted = glitchConsumerPresent();
            Log::get().note(
                "transition flash: frame %u was drawn from %.0f world units off the "
                "camera's path (threshold %.0f), at (%+.0f %+.0f %+.0f) with %u eye "
                "draws. %s %u detected this session.",
                s->frameNo, s->lastResid, s->lastTrip, s->frameFarPos[0],
                s->frameFarPos[1], s->frameFarPos[2], gateDraws,
                acted ? "Withheld; SteamVR will reproject the previous frame."
                      : "NOT withheld -- openvr_api.dll is not installed, so nothing "
                        "was in a position to stop it being shown.",
                s->framesWithheld);
        }
    } else if (s->jumpedThisFrame) {
        unmarkGlitchFrame();
    }

    // Did the camera COME BACK?
    //
    // This is what separates the glitch from a legitimate change of reference
    // frame, and it is the distinction the glitch is defined by: a glitch leaves
    // the path for one frame and returns; arriving at a station or opening a map
    // moves the whole coordinate system and STAYS there.
    //
    // It cannot be known when the jump happens, only afterwards, so the first
    // frame of a re-basing is withheld either way. What this prevents is the
    // other ten: station arrivals produced bursts of eleven and twelve withheld
    // frames, and at roughly 80 ms each that is most of a second of judder --
    // worse than the flash it was trying to remove.
    if (s->awaitingReturn && s->frameFarMag2 >= 0.0f) {
        s->awaitingReturn = false;
        float back = 0.0f;
        for (uint32_t a = 0; a < 3; ++a) {
            const float pred =
                s->preJumpPrev[a] + 2.0f * (s->preJumpPrev[a] - s->preJumpPrev2[a]);
            const float e = s->frameFarPos[a] - pred;
            back += e * e;
        }
        back = sqrtf(back);
        if (back <= s->lastTrip) {
            // Returned. A one-frame excursion, so the pre-jump path is still the
            // right one and detection carries on immediately.
            for (uint32_t a = 0; a < 3; ++a) {
                s->camPrev2[a] = s->preJumpPrev[a];
                s->camPrev[a] = s->frameFarPos[a];
            }
            s->camPrevValid = 2;
        } else {
            // Stayed. The reference frame moved, so further jumps here are the
            // same event and withholding them buys nothing but judder.
            s->cooldown = kRebaseCooldown;
            s->camPrevValid = 0;
            if (!s->rebaseNoted) {
                s->rebaseNoted = true;
                Log::get().note(
                    "transition flash: the camera did not return after a jump (%.0f "
                    "units off the old path), so that was a change of reference frame "
                    "rather than a bad frame. Standing down for %u frames instead of "
                    "withholding the rest of it.",
                    back, kRebaseCooldown);
            }
        }
    } else if (s->jumpedThisFrame) {
        for (uint32_t a = 0; a < 3; ++a) {
            s->preJumpPrev[a] = s->camPrev[a];
            s->preJumpPrev2[a] = s->camPrev2[a];
        }
        s->awaitingReturn = true;
        // The path is broken, so the prediction is discarded and rebuilt from
        // the next two frames. Keeping it anchored to the pre-jump position left
        // every following frame far from a stale guess and fired on all of them.
        s->camPrevValid = 0;
    } else if (s->frameFarMag2 >= 0.0f) {
        for (uint32_t a = 0; a < 3; ++a) {
            s->camPrev2[a] = s->camPrev[a];
            s->camPrev[a] = s->frameFarPos[a];
        }
        if (s->camPrevValid < 2) ++s->camPrevValid;
    }

    // Record the frame, whether or not anything was wrong with it. The value of
    // the history is the frames either side of an event, not the event alone.
    if (s->frameFarMag2 >= 0.0f) {
        RingEntry& e = s->ring[s->ringHead % kRingFrames];
        e.qpc = static_cast<uint64_t>(qpcNow());
        e.frame = s->frameNo;
        e.eyeDraws = eyeDraws;
        for (uint32_t a = 0; a < 3; ++a) e.pos[a] = s->frameFarPos[a];
        ++s->ringHead;
    }

    // Runaway guard. A detector that withholds frames continuously is wrong
    // about the scene, and the failure mode -- permanent judder -- is worse than
    // the flash it exists to remove.
    if (++s->windowFrames >= kRunawayWindow) {
        if (s->windowWithheld > kRunawayLimit) {
            s->disabledForSession = true;
            Log::get().note(
                "transition flash fix DISABLED for this session: %u of the last %u "
                "frames were withheld, far more than the handful of transitions any "
                "session contains. Whatever it is watching is not the camera, or this "
                "scene breaks the assumption it rests on. The game renders normally "
                "from here; please report this with the log.",
                s->windowWithheld, s->windowFrames);
        }
        s->windowFrames = 0;
        s->windowWithheld = 0;
    }

    if (s->markedThisFrame) ++s->consecutive; else s->consecutive = 0;
    if (s->cooldown > 0) --s->cooldown;
    s->markedThisFrame = false;
    s->jumpedThisFrame = false;
    s->frameFarMag2 = -1.0f;
}

void dumpCameraRing() {
    State* s = g_state;
    if (!s) return;
    if (!s->enabled) {
        Log::get().note("camera history dump requested, but the transition flash fix is "
                        "off, so nothing has been recorded.");
        return;
    }
    if (s->ringHead == 0) {
        Log::get().note("camera history dump requested, but nothing has been recorded "
                        "yet: no frame has had a scene camera bound to its draws.");
        return;
    }

    const uint64_t have = s->ringHead < kRingFrames ? s->ringHead : kRingFrames;
    const uint64_t first = s->ringHead - have;
    const int64_t freq = qpcFrequency();
    const uint64_t newest = s->ring[(s->ringHead - 1) % kRingFrames].qpc;

    Log::get().note(
        "--- camera history: %llu frames, oldest first. Columns are milliseconds "
        "before the dump, frame number, draws that reached the eye textures, and the "
        "camera position. A bad frame is one whose position leaves the line the frames "
        "either side of it are following, and returns. %u frame(s) withheld so far this "
        "session. ---",
        static_cast<unsigned long long>(have), s->framesWithheld);

    for (uint64_t i = first; i < s->ringHead; ++i) {
        const RingEntry& e = s->ring[i % kRingFrames];
        const double msAgo =
            freq ? static_cast<double>(static_cast<int64_t>(newest - e.qpc)) * 1000.0 /
                       static_cast<double>(freq)
                 : 0.0;
        Log::get().note("CAM %8.1fms f%-7u eye=%-5u pos=(%+.2f %+.2f %+.2f)",
                        -msAgo, e.frame, e.eyeDraws, e.pos[0], e.pos[1], e.pos[2]);
    }
    Log::get().note("--- end camera history ---");
}

void shutdownGlitchFrameFix() {
    State* s = g_state;
    if (!s || !s->enabled) return;
    if (s->disabledForSession) {
        Log::get().note("transition flash fix: disabled during the session, see above.");
    } else if (!glitchConsumerPresent()) {
        // Says DETECTED, never "withheld". This half cannot withhold anything on
        // its own, and a summary claiming otherwise would tell somebody who
        // skipped the second file that the fix had been working all along.
        Log::get().note(
            "transition flash fix: %u bad frame(s) detected this session, and NONE of "
            "them were withheld -- openvr_api.dll is not installed, so the detection "
            "had nothing to act on. Everything else in EDVR works without it; only "
            "this fix needs it. See the openvr folder in the download.",
            s->framesWithheld);
    } else {
        Log::get().note(
            "transition flash fix: %u frame(s) withheld this session. Compare that "
            "against how many jumps, drops and map closes happened: roughly one per "
            "transition is it working, many more means it is firing on something else. "
            "The openvr log counts the same frames from the other side and should read "
            "exactly twice this, once per eye.",
            s->framesWithheld);
    }
    g_state = nullptr;
}

}  // namespace edvr
