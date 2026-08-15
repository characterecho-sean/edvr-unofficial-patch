// GENERATED from src/d3d11/glitch_frame.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 915db1e959285328]
#include "glitch_frame.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
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

// Distinct jump magnitudes remembered, so a magnitude that RECURS can be told
// from one that does not.
//
// WHAT THIS IS FOR (EVIDENCE 6v, and the field session that made it matter)
//
// The signal this detector watches is the frame's furthest-from-origin camera,
// and 6v established that which camera that is depends on which render passes
// ran -- not on where the view is. A frame that drew shadow cascades reports a
// camera hundreds of thousands of units from the one a lighter frame reports,
// and the "jump" between them is the FIXED SEPARATION between two cascades. It
// recurs to within a fraction of a percent, forever, while the player flies in
// a straight line.
//
// Measured on a planet surface, one session, forty logged jumps: thirty-eight
// between 562,949 and 571,365 units -- a spread of 1.5% -- and two genuine
// transitions at 7,311 and 8,735. The detector withheld all forty, in bursts of
// nine inside a second, until the runaway guard disabled it for the session.
// That is the rhythmic judder the field report described, and the fix taking
// itself off the field with it.
//
// A repeating magnitude is therefore not a transition. 6v put it exactly: an
// exactly repeating residual is what a fixed separation produces and what
// genuine motion does not.
constexpr uint32_t kSeparations = 16;

// A magnitude counts as the same one within this fraction of itself.
//
// RELATIVE, not absolute, and the difference is not cosmetic. 6v's cascade pair
// repeated within 51 units at 146,000 -- one part in 2,900 -- which makes an
// absolute tolerance of a few hundred look generous. The pair measured in the
// field spans 8,416 units at 568,000, which the same tolerance would have missed
// entirely, and the whole fix with it.
//
// 2% is chosen against the case that must NOT be suppressed. The one session
// holding both kinds gives fourteen genuine transition residuals -- 2,297,
// 2,468, 2,609, 3,010, 3,638, 4,220, 5,408, 5,882, 6,070, 6,856, 8,644, 11,195,
// 11,906, 12,477 -- whose closest pair, 2,468 and 2,609, is 5.7% apart. Real
// flashes vary because real motion varies; separations do not.
constexpr float kDefaultRepeatPercent = 2.0f;

// Rebase notes, which used to be one per SESSION.
//
// `rebaseNoted` latched on the first one, so every later change of reference
// frame -- a different magnitude, a different cause, possibly the one somebody
// is reporting -- was silent, and the last line in the log described an event
// minutes earlier. A rebase already sets kRebaseCooldown, so they cannot burst;
// the latch was suppressing nothing but information.
//
// Eight was wrong for a different reason, and "they cannot burst" was simply
// false: one mode change produced eight of them inside 400 ms, which spent the
// whole session's budget on a single event and left every stand-down afterwards
// silent. Each costs 120 frames during which nothing can be withheld, and that
// is precisely where a real flash gets through -- so these are the lines worth
// keeping. Matched to the jump-note budget.
constexpr uint32_t kRebaseNotes = 40;

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

// How often the running totals are printed.
//
// THE END OF THE SESSION IS NOT A PLACE TO PUT NUMBERS ANYBODY WILL SEE.
//
// shutdownGlitchFrameFix has never once logged in the field, across every
// session this fix has been debugged from. It is reached only from shutdown(),
// and DllMain calls that only when `reserved` is null -- a FreeLibrary unload,
// which does not happen to a game's d3d11.dll. Real exits take the other branch,
// where Windows has already terminated every other thread, possibly while one
// held the log's spinlock. Anything there that takes a lock risks hanging the
// process rather than losing a line, which is why that branch is deliberately
// minimal, and it should stay that way: the fault is not that exit does too
// little, it is that the numbers were put somewhere only exit could reach.
//
// Five sessions of bug-hunting wanted "withheld versus recognised" and no log
// has ever carried it. The population that most needs a summary is exactly the
// one whose game did not exit cleanly. vScreen has printed its totals on a timer
// since long before anybody noticed this; this is the same answer.
constexpr uint32_t kTotalsEvery = 1800;

// Below this magnitude, a camera is not a WORLD camera and the frame carries no
// reading at all.
//
// Elite renders camera-relative (6t): the main view's transform sits in head
// space at magnitude about 0.09 (6v.5), and its world position never reaches a
// constant buffer. The only world-space cameras in a frame belong to auxiliary
// passes, and those are measured in thousands to hundreds of thousands -- 9,967
// and 136,405 in 6v, and 5,012 / 7,600 / 568,000 in the field.
//
// So a frame whose FURTHEST camera is at the origin is a frame in which no
// auxiliary pass ran, not a frame drawn from the origin. The "jump" it produces
// is the distance from the previous frame's world camera to zero -- pure pass
// composition, exactly 6v, and invisible to the repeat suppression because that
// distance is whatever the last world camera happened to be and so never repeats.
//
// Measured three times, each a good frame withheld: frame 11774 at 7,618 units
// "from" (-0 -0 +0), frame 17613 at 10,277 from (-0 +0 +0), frame 28667 at 6,856
// from (+0 +0 -0). The first of those is the frame a player reported seeing a
// flash on, in a log that ends 89 ms later.
//
// Ten units. Not a tuned number: it sits two decades above the head-space
// magnitudes it must exclude and nearly three below the smallest world camera
// ever recorded, in a gap four decades wide.
constexpr float kWorldCameraFloor2 = 10.0f * 10.0f;

// THE SECOND INVARIANT: a camera orbiting at a fixed radius.
//
// The separation memory above keys on JUMP SIZE, and there is a shape it cannot
// see. Six of ten remaining withholds in one session sat at wildly different
// directions but one radius -- |pos| 7,609 to 7,635, inside 0.3% -- an auxiliary
// camera at a fixed distance from the view origin, swinging as the player turns.
// The jump size varies with where the swing came from, so the separation memory
// caught two of the six. The radius does not vary. 6v.2 recorded the same
// signature at 136,405.
//
// WHY THIS ONE IS STRONGER THAN THE ONE IT JOINS. A sphere is visible on every
// frame the camera is sampled, not only at jumps -- so it can be certified by
// watching, before it has ever caused a mark. The separation memory pays one
// withheld frame to learn each magnitude; this learns for free while the player
// flies. The radius is already computed on the hot path.
//
// CERTIFICATION IS WHAT KEEPS IT HONEST. A radius seen once is a coincidence; a
// radius seen for a third of a second in three provably different directions is
// a sphere, and a one-frame excursion cannot certify by construction. Both
// halves are required: sightings alone would certify a parked camera, directions
// alone would certify noise.
// SIXTY-FOUR, and eight was badly wrong -- my number, not the design's.
//
// A ten-second ring from one session carries twelve or more radii in constant
// use: 68,900 on 440 of 900 frames, then 7,000, 6,900, 7,100, 67,200, 7,200,
// 8,000, 7,700, 7,500, 9,700, 8,800, 9,800. Against eight slots that table does
// nothing but evict, and every eviction throws away a certification and makes
// the next sighting of that radius start from nothing.
//
// The symptom in the log is unmistakable once you know to look: 29 shells
// certified in five minutes, five of them at 6,861 / 6,868 / 6,879 / 6,882 /
// 6,885 -- all inside the 0.5% tolerance of each other, so all the SAME shell,
// learned and lost and learned again. Meanwhile the split counter read 460
// suppressed by jump size against 24 by radius: the invariant was barely getting
// to work, and four of the ten frames still being withheld sat on radii the
// table had already identified and forgotten.
//
// The cost of the larger table is a linear scan of floats on the observe path,
// which is nothing next to the sqrtf already there.
constexpr uint32_t kShells = 64;
constexpr uint32_t kShellCertifyFrames = 30;
constexpr uint32_t kShellCertifyDirs = 3;
// cos 15 degrees: how far the camera must swing to count as a new direction.
constexpr float kShellDirCos = 0.966f;

// A FRACTION, not a percentage -- unlike transition_flash_repeat_percent beside
// it, which is one. The two are documented together in the ini for that reason.
//
// Tight, and it has to be: the measured spread is 0.3% at 7.6k and 6v held to
// about 0.005% at 136k, while the genuine-flash band (9,900 to 24,000 off path)
// sits adjacent to this session's shell. A thick shell would start swallowing
// the thing this exists to catch.
constexpr float kDefaultRadiusTolerance = 0.005f;

// THE THIRD INVARIANT: a camera that does not move at all.
//
// Two withholds at radius ~69,000 landed four units apart -- (-33141 -32695
// -50433) and (-33140 -32695 -50437). The sphere refused to certify it, exactly
// as it must: one direction is a parked camera, and certifying that would let
// any stationary pass switch the detector off. The separation memory should have
// taken it and missed by 0.05 of a percentage point -- 73,460 against 71,958 is
// 2.05% apart against a 2% window.
//
// The answer is not to widen a tolerance on one data point. It is to key on what
// actually recurred, which is neither the jump size nor the distance: it is the
// DESTINATION. All three invariants are the same statement -- the track landed
// where auxiliary geometry has landed before -- and the separation memory, keyed
// on jump size, is the weakest form of it because its key varies with where the
// jump started. Both field misses trace to exactly that.
//
// ABSOLUTE, not relative, and that is the point of it being a separate rule: the
// observed park held to four units at a radius of 69,000, which no percentage
// worth having would express. Sixty-four units keeps an order of magnitude of
// slack over the measurement.
//
// A park and an orbit are complementary by construction and an entry may certify
// as only one: the orbit needs three directions, the park needs the position to
// hold still. Whichever is proven first is what that entry is.
constexpr float kDefaultParkUnits = 64.0f;

// Consecutive frames on a radius that prove the radius belongs to the view.
//
// PASSES VISIT; THE VIEW STAYS. See the long note in observeShell for what this
// is defending against and why it applies to shells and never to parks.
constexpr uint32_t kDefaultDwellFrames = 20;

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
    // enabled    the fix may withhold frames
    // observing  the viewpoint history is being recorded
    //
    // TWO THINGS, and they were one. `enabled` gated both, so
    // fix.transition_flash = 0 took the instrument down with the fix -- and that
    // configuration is precisely the control a bug report needs: with nothing
    // withheld, anything still seen is definitively not EDVR's. Asking for it
    // produced "camera history dump requested, but the transition flash fix is
    // off, so nothing has been recorded", which is the instrument refusing to
    // measure the one case it was built to settle.
    //
    // The file already half-believed this: it goes on recording after the
    // detector stands down mid-session, with a comment saying the dump must
    // describe the moment the user pressed the key rather than the moment the
    // fix gave up. That reasoning simply never reached `enabled`.
    //
    // Observation costs a size compare per Map, a twelve-byte read, one ring
    // write a frame, and the eye-draw count. Only somebody who has turned the
    // fix off pays it, and what they get for it is a usable bug report.
    bool     enabled = false;
    bool     observing = false;
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
    uint32_t rebaseNotesLeft = kRebaseNotes;

    // Jump magnitudes seen before. See kSeparations.
    struct Separation {
        float    resid = 0.0f;
        uint32_t lastSeen = 0;
        uint32_t hits = 0;
        bool     noted = false;
    };
    Separation seps[kSeparations] = {};
    float      repeatPercent = kDefaultRepeatPercent;

    // Radii the furthest camera keeps landing on. See kShells.
    struct Shell {
        float    radius = 0.0f;
        uint32_t lastSeen = 0;
        uint32_t framesSeen = 0;
        float    lastDir[3] = {};
        uint32_t distinctDirs = 0;
        bool     certified = false;      // an orbit: one distance, many bearings

        // TRANSIENCE. How long the track has sat on this radius without leaving.
        //
        // PASSES VISIT; THE VIEW STAYS. The spatial invariants say WHERE geometry
        // repeats; dwell says WHOSE it is, and it is the premise the shell rule
        // was missing. Measured on both sides: 6v's cascade blocks rest 4-6
        // frames, the view rests 100+ (6ah). Anything that stays is the view.
        uint32_t dwellRun = 0;
        uint32_t maxDwell = 0;
        bool     dwellDisqualified = false;   // permanent, and revokes
        bool     refusedNoted = false;

        // The parked variant, on the same record rather than in a table of its
        // own: a park has a fixed position and therefore a fixed radius, so the
        // radius lookup already finds it and only the position needs checking.
        // The shape tag is which of the two certifications it holds.
        float    parkPos[3] = {};
        uint32_t parkFrames = 0;
        bool     parked = false;         // a point: one place, over and over

        // THE CERTIFIED POINT, FROZEN, and separate from the run tracker above.
        //
        // parkPos is a CANDIDATE: it follows the camera, and observeShell moves it
        // to whatever position arrives whenever the run breaks. The suppression
        // test used to read it, and observeShell runs BEFORE the decision in the
        // same frame -- so by the time "is this position the parked one?" was
        // asked, parkPos had already been rewritten to the position being asked
        // about. It compared the frame against itself and answered yes.
        //
        // The effect was that ANY position sharing a parked record's radius band
        // was exempt, not just the parked point. Measured on the replay of the
        // 2026-08-15 wake capture: the real bad frame, 9,445 units from the view
        // and nowhere near any parked camera, came back park-suppressed.
        float    certParkPos[3] = {};
    };
    Shell      shells[kShells] = {};
    float      radiusTolerance = kDefaultRadiusTolerance;
    float      parkUnits = kDefaultParkUnits;
    uint32_t   dwellFrames = kDefaultDwellFrames;

    uint32_t   suppressed = 0;
    // Split, because which invariant did the work is the data that says whether
    // both are earning their place -- and whether the deferred per-source-track
    // idea is ever needed.
    uint32_t   suppressedByRadius = 0;
    uint32_t   suppressedByPark = 0;
    // The frame of the most recent withhold, so a history dump can say whether
    // anything in it was ours. See dumpCameraRing.
    uint32_t   lastWithheldFrame = 0;
    uint32_t   suppressedBySeparation = 0;
    bool       radiusSuppressedThisFrame = false;
    bool       parkSuppressedThisFrame = false;
    // Periodic totals: when they were last printed, and what they said. Printed
    // only when a counter has moved, so a quiet session stays quiet.
    uint32_t   totalsAt = 0;
    uint32_t   totalsWithheld = 0;
    uint32_t   totalsSuppressed = 0;
    // Set by the detector, read by the boundary: this frame's jump matched a
    // magnitude already known to recur, so it is a pass flip and not a frame to
    // withhold.
    bool       suppressedThisFrame = false;

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

// The remembered magnitude matching `resid`, or -1.
//
// Stale entries do not match. A separation that has not been seen for a runaway
// window is no longer characteristic of the scene being rendered -- the player
// has flown somewhere else -- and holding it forever would let one afternoon's
// cascade geometry suppress a genuine flash an hour later.
int findSeparation(float resid) {
    State* s = g_state;
    if (s->repeatPercent <= 0.0f) return -1;
    // GUARDED HERE, not only at the caller, because infinity does not fail this
    // comparison -- it passes it. `fabsf(anything - inf) <= inf` is true in IEEE,
    // so an infinite residual matches whichever entry it reaches first, and
    // recordResidual then drags that entry to infinity through the running mean.
    // One garbage frame destroys a separation learned over a minute of flight.
    //
    // The caller refuses non-finite residuals too. This is here because a memory
    // whose correctness depends on every future caller remembering to filter its
    // input is a memory that will be poisoned eventually.
    if (!std::isfinite(resid)) return -1;
    const float tol = resid * (s->repeatPercent * 0.01f);
    for (uint32_t i = 0; i < kSeparations; ++i) {
        if (s->seps[i].hits == 0) continue;
        if (s->frameNo - s->seps[i].lastSeen > kRunawayWindow) continue;
        if (fabsf(s->seps[i].resid - resid) <= tol) return static_cast<int>(i);
    }
    return -1;
}

// Pure. Kept separate from recording on purpose: the detector re-decides on
// every new furthest camera within a frame, so a query that also counted a hit
// would inflate the count by however many candidates that frame happened to
// have. Recording happens once, at the frame boundary.
bool residualIsKnownSeparation(float resid) { return findSeparation(resid) >= 0; }

// Remember a magnitude, or refresh one already remembered.
//
// Refreshing matters as much as inserting. Without it a separation that is
// suppressing correctly ages out after a runaway window and fires again, which
// is the same judder at a longer period.
void recordResidual(float resid) {
    State* s = g_state;
    if (s->repeatPercent <= 0.0f) return;
    if (!std::isfinite(resid)) return;   // never let one into the table
    const int hit = findSeparation(resid);
    if (hit >= 0) {
        State::Separation& e = s->seps[hit];
        ++e.hits;
        e.lastSeen = s->frameNo;
        // FOLLOW THE MAGNITUDE. It is not actually fixed -- it drifts as the
        // player flies, and an entry pinned to its first sighting gets left
        // behind while the thing it is tracking walks away.
        //
        // Measured: 563,308 then 548,012 then 532,120 over four minutes, each
        // 2.7-2.9% from the last. Every one fell outside its predecessor's 2%
        // window, opened a new entry and cost a frame -- while all the sightings
        // in between were matching correctly and refreshing an entry that was
        // being held at a number nothing had reported for a minute.
        //
        // A running mean follows it, and the weight is not a taste: for a drift
        // of d per sighting the mean settles a fixed fraction d/(w-d) behind, so
        // it has to stay inside the match window. At w = 0.25 a 1%-per-sighting
        // drift lags 4.2% and falls straight out of a 2% window -- measured, in
        // the test below, which is why the weight is a half and not a quarter.
        //
        // Half is also the most an outlier can move the entry, and only values
        // already INSIDE the window are ever applied, so one reading can shift
        // this by at most 1%. It cannot be walked somewhere by noise.
        //
        // Widening the tolerance instead would buy the same thing and is the
        // wrong trade: it spends the margin against real flashes, which is the
        // only thing standing between this and suppressing the bug it exists to
        // fix.
        e.resid += (resid - e.resid) * 0.5f;
        // Once per magnitude, not once per suppressed frame -- there are
        // thousands of those and one of these.
        if (!e.noted && e.hits >= 2) {
            e.noted = true;
            Log::get().note(
                "transition flash: a jump of about %.0f world units has now "
                "happened %u times. A repeating magnitude is a fixed separation "
                "between two of the game's render passes, not a transition -- so "
                "frames matching it are no longer being withheld. This is the "
                "detector recognising the scene, not a fault.",
                static_cast<double>(e.resid), e.hits);
        }
        return;
    }
    // A free slot, or the least recently seen.
    uint32_t victim = 0;
    for (uint32_t i = 0; i < kSeparations; ++i) {
        if (s->seps[i].hits == 0) { victim = i; break; }
        if (s->seps[i].lastSeen < s->seps[victim].lastSeen) victim = i;
    }
    s->seps[victim] = State::Separation{};
    s->seps[victim].resid = resid;
    s->seps[victim].lastSeen = s->frameNo;
    s->seps[victim].hits = 1;
}

// The remembered shell matching this radius, or -1. Stale entries do not match,
// for the same reason separations do not: a graphics change that moves a cascade
// distance should cost one frame, not a session of wrong answers.
int findShell(float radius) {
    State* s = g_state;
    if (s->radiusTolerance <= 0.0f) return -1;
    if (!std::isfinite(radius) || radius <= 0.0f) return -1;
    const float tol = radius * s->radiusTolerance;
    for (uint32_t i = 0; i < kShells; ++i) {
        if (s->shells[i].framesSeen == 0) continue;
        if (s->frameNo - s->shells[i].lastSeen > kRunawayWindow) continue;
        if (fabsf(s->shells[i].radius - radius) <= tol) return static_cast<int>(i);
    }
    return -1;
}

// Only a CERTIFIED shell suppresses. An entry still gathering evidence is a
// hypothesis, and acting on a hypothesis is how the first version of the
// separation memory came to withhold eleven frames in a row.
bool radiusIsCertifiedShell(float radius) {
    const int i = findShell(radius);
    return i >= 0 && g_state->shells[i].certified;
}

// Squared distance between two points, which both park tests want.
float dist2(const float* a, const float* b) {
    float d2 = 0.0f;
    for (uint32_t i = 0; i < 3; ++i) {
        const float d = a[i] - b[i];
        d2 += d * d;
    }
    return d2;
}

// Did the track land where a certified parked camera sits?
//
// Found by radius first -- a fixed point has a fixed distance, so the same
// lookup serves -- and then confirmed by position, because two different parked
// cameras can share a radius and only one of them is this one.
bool positionIsCertifiedPark(const float* pos, float radius) {
    State* s = g_state;
    if (s->parkUnits <= 0.0f) return false;
    const int i = findShell(radius);
    if (i < 0 || !s->shells[i].parked) return false;
    return dist2(pos, s->shells[i].certParkPos) <= s->parkUnits * s->parkUnits;
}

// Learn from a camera position, whether or not it produced a jump.
//
// This is the half that costs nothing: every accepted world camera teaches the
// table, so by the time a swing produces a mark the sphere is usually already
// certified and the mark never happens.
void observeShell(const float* pos, float radius) {
    State* s = g_state;
    if (s->radiusTolerance <= 0.0f) return;
    if (!std::isfinite(radius) || radius <= 0.0f) return;

    int hit = findShell(radius);
    if (hit < 0) {
        uint32_t victim = 0;
        for (uint32_t i = 0; i < kShells; ++i) {
            if (s->shells[i].framesSeen == 0) { victim = i; break; }
            if (s->shells[i].lastSeen < s->shells[victim].lastSeen) victim = i;
        }
        s->shells[victim] = State::Shell{};
        s->shells[victim].radius = radius;
        hit = static_cast<int>(victim);
    }
    State::Shell& e = s->shells[hit];

    // ONCE PER FRAME, however many cameras that frame carried. Counting per
    // write would let a single heavy frame with a dozen passes on one shell
    // certify it outright, which is not a third of a second of evidence -- it is
    // one frame wearing a disguise.
    if (e.framesSeen > 0 && e.lastSeen == s->frameNo) return;

    // TRANSIENCE, TESTED BEFORE lastSeen IS OVERWRITTEN.
    //
    // PASSES VISIT; THE VIEW STAYS. Every other test in this file asks WHERE the
    // geometry is -- a repeating jump size, a recurring radius, a fixed point --
    // and none of them can say whose geometry it is. This one can, and it is the
    // premise the shell rule shipped without.
    //
    // THE FAILURE IT EXISTS TO PREVENT (EVIDENCE 6ai, low wake capture of
    // 2026-08-15): a shell certified at radius 6864, and that radius was the
    // player. 38% of every frame recorded that session sat inside the 0.5% band;
    // the most-seen positions in it were the ordinary flight positions, held for
    // a hundred consecutive frames at a time; the median radius over 900 frames
    // of flight was 6872. A ship flying near a body holds a near-constant
    // distance from its centre while its bearing changes continuously -- it
    // performs the orbit the certification was built to treat as aux-only. And
    // the damage was not theoretical: one of the two bad frames in that capture
    // sat 0.37% off the certified radius, inside the band and therefore exempt,
    // certified 0.3 s before it happened.
    //
    // SHELLS ONLY, AND THE SYMMETRY IS EXACT. A park REQUIRES rest -- 1c's
    // parked cascade legitimately sits at one point for thirty frames, and the
    // 69k park is the proof -- so prohibiting rest at a POINT would undo that
    // fix. Park: rest at a place, required. Shell: rest on a radius, forbidden.
    // The difference is that a point is a place geometry can honestly sit, while
    // a radius is a place only the thing being drawn from can honestly live on.
    if (e.framesSeen > 0 && e.lastSeen + 1 == s->frameNo) {
        ++e.dwellRun;
    } else {
        e.dwellRun = 1;
    }
    if (e.dwellRun > e.maxDwell) e.maxDwell = e.dwellRun;

    if (!e.dwellDisqualified && e.dwellRun >= s->dwellFrames) {
        e.dwellDisqualified = true;
        // REVOKED, not merely refused. A shell certified far from anything can
        // later collide with a flight radius near a body -- the session is long
        // and the ship goes places -- so a certification has to be able to lose
        // its evidence as well as gain it. Permanent for this entry either way:
        // once the track has been shown to live here, later transience is the
        // artefact moving through, not proof the view left.
        const bool wasCertified = e.certified;
        e.certified = false;
        if (!e.refusedNoted) {
            e.refusedNoted = true;
            Log::get().note(
                "transition flash: radius %.0f %s -- the camera has now sat on it "
                "for %u frames without leaving, and a render pass behind the view "
                "visits a distance, it does not live at one. A ship near a planet "
                "holds a constant distance from its centre while its bearing "
                "changes; that is flight, not an artefact. Jumps landing on this "
                "radius are judged normally%s.",
                static_cast<double>(e.radius),
                wasCertified ? "is no longer treated as an auxiliary render pass"
                             : "will not be certified as an auxiliary render pass",
                e.dwellRun, wasCertified ? " again" : "");
        }
    }

    e.lastSeen = s->frameNo;
    ++e.framesSeen;
    e.radius += (radius - e.radius) * 0.25f;

    const float inv = 1.0f / radius;
    const float dir[3] = {pos[0] * inv, pos[1] * inv, pos[2] * inv};
    if (e.distinctDirs == 0) {
        for (uint32_t a = 0; a < 3; ++a) e.lastDir[a] = dir[a];
        e.distinctDirs = 1;
    } else {
        const float dot = dir[0] * e.lastDir[0] + dir[1] * e.lastDir[1] +
                          dir[2] * e.lastDir[2];
        if (dot < kShellDirCos) {
            for (uint32_t a = 0; a < 3; ++a) e.lastDir[a] = dir[a];
            ++e.distinctDirs;
        }
    }

    // The parked variant, tracked alongside. A run of sightings at one place
    // certifies a point; anything that moves further than the tolerance starts
    // the run again, so a camera that drifts never qualifies as parked.
    if (s->parkUnits > 0.0f) {
        if (e.parkFrames > 0 && dist2(pos, e.parkPos) <= s->parkUnits * s->parkUnits) {
            ++e.parkFrames;
        } else {
            for (uint32_t a = 0; a < 3; ++a) e.parkPos[a] = pos[a];
            e.parkFrames = 1;
        }
        // ONE CERTIFICATION PER ENTRY. `distinctDirs <= 1` is the complement of
        // the orbit's gate, so a record cannot be both -- and `!e.certified`
        // makes whichever was proven first the answer, rather than letting a
        // camera that parks after orbiting quietly change shape.
        if (!e.parked && !e.certified && e.parkFrames >= kShellCertifyFrames &&
            e.distinctDirs <= 1) {
            e.parked = true;
            // Frozen here, at the moment the evidence is complete, and never
            // touched again. The candidate above goes on following the camera.
            for (uint32_t a = 0; a < 3; ++a) e.certParkPos[a] = e.parkPos[a];
            Log::get().note(
                "transition flash: a camera parked at (%+.0f %+.0f %+.0f) identified "
                "(%u sightings without moving) -- an auxiliary render pass being "
                "resampled, not the view. Frames that land on it are no longer "
                "withheld. A view that has not moved in a third of a second is not "
                "a view that just jumped.",
                e.parkPos[0], e.parkPos[1], e.parkPos[2], e.parkFrames);
        }
    }

    if (!e.certified && !e.parked && !e.dwellDisqualified &&
        e.framesSeen >= kShellCertifyFrames &&
        e.distinctDirs >= kShellCertifyDirs) {
        e.certified = true;
        Log::get().note(
            "transition flash: a camera orbiting at radius %.0f identified (%u "
            "sightings in %u distinct directions, never resting on it for more "
            "than %u frames) -- an auxiliary render pass, not the view. Frames "
            "whose camera lands on it are no longer withheld. A view does not "
            "hold one distance while it swings AND keep moving off it.",
            static_cast<double>(e.radius), e.framesSeen, e.distinctDirs,
            e.maxDwell);
    }
}

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
    // Clamped rather than trusted, and 0 is a real setting rather than an error:
    // it turns the suppression off, which is the escape hatch if a build ever
    // produces genuine flashes that coincidentally repeat. A negative or absurd
    // value is a typo, and the direction of the mistake matters -- too large a
    // tolerance suppresses real flashes, which is silent.
    const float repeat = cfg.getFloat("advanced.transition_flash_repeat_percent",
                                      kDefaultRepeatPercent);
    if (!std::isfinite(repeat) || repeat < 0.0f || repeat > 50.0f) {
        Log::get().note(
            "transition_flash_repeat_percent = %.2f is outside 0 to 50, so %.1f is "
            "being used. This is how close two jump magnitudes have to be for the "
            "second to be treated as the same fixed separation; too large and real "
            "flashes get suppressed, which nothing would tell you.",
            static_cast<double>(repeat), static_cast<double>(kDefaultRepeatPercent));
        s.repeatPercent = kDefaultRepeatPercent;
    } else {
        s.repeatPercent = repeat;
    }
    // A FRACTION here, where the setting above it is a percentage. That is not
    // an oversight but it IS a trap, so the ini says so beside both of them.
    const float radiusTol = cfg.getFloat("advanced.transition_flash_radius_tolerance",
                                         kDefaultRadiusTolerance);
    if (!std::isfinite(radiusTol) || radiusTol < 0.0f || radiusTol > 0.05f) {
        Log::get().note(
            "transition_flash_radius_tolerance = %.4f is outside 0 to 0.05, so %.4f "
            "is being used. It is a FRACTION -- 0.005 is half a percent -- and it is "
            "how close two camera distances have to be to count as the same orbit. "
            "Too wide and a genuine bad frame that happens to land near one gets "
            "treated as the pass and shown.",
            static_cast<double>(radiusTol),
            static_cast<double>(kDefaultRadiusTolerance));
        s.radiusTolerance = kDefaultRadiusTolerance;
    } else {
        s.radiusTolerance = radiusTol;
    }
    // ABSOLUTE units, where the two settings above it are proportional. The ini
    // says so beside all three; a park is a fixed point and a percentage of a
    // 69,000-unit radius would be hundreds of units of slack on a camera that
    // was measured holding still to four.
    const float parkUnits = cfg.getFloat("advanced.transition_flash_park_units",
                                         kDefaultParkUnits);
    if (!std::isfinite(parkUnits) || parkUnits < 0.0f || parkUnits > 1000.0f) {
        Log::get().note(
            "transition_flash_park_units = %.1f is outside 0 to 1000, so %.0f is "
            "being used. It is how still a camera has to hold, in world units, to "
            "count as parked.",
            static_cast<double>(parkUnits), static_cast<double>(kDefaultParkUnits));
        s.parkUnits = kDefaultParkUnits;
    } else {
        s.parkUnits = parkUnits;
    }
    // How long a camera has to sit on a radius before that radius is the view's.
    //
    // Twenty frames, about a fifth of a second, and it is bracketed by
    // measurement rather than chosen between guesses: 6v's cascade blocks rest
    // four to six frames, and the view rests a hundred or more (6ah). Anything
    // in the wide gap between separates them. The floor of 8 keeps it clear of
    // the cascades; the ceiling of 120 keeps it under what the view does, since
    // a value above that would never fire and would silently restore the bug.
    s.dwellFrames = static_cast<uint32_t>(cfg.getIntInRange(
        "advanced.transition_flash_dwell_frames", kDefaultDwellFrames, 8, 120));
    s.bufferBytes = static_cast<uint32_t>(cfg.getInt("advanced.camera_buffer_bytes", 5376));
    s.posOffset = static_cast<uint32_t>(cfg.getInt("advanced.camera_buffer_offset", 1100));

    if (!s.enabled) {
        // Off, but still watching. See State::observing.
        if (static_cast<uint64_t>(s.posOffset) * 4u + 12u <= s.bufferBytes &&
            s.bufferBytes != 0) {
            s.observing = true;
            Log::get().note(
                "transition flash fix off (fix.transition_flash = 0). No frame will "
                "be withheld. The viewpoint history is still being recorded, so the "
                "history key still works -- which makes this the clean control for "
                "reporting a flash: with nothing withheld, whatever you saw was not "
                "EDVR.");
        } else {
            Log::get().note("transition flash fix off (fix.transition_flash = 0)");
        }
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
    // The geometry is sound, so there is somewhere to watch. Set on BOTH paths
    // through this function -- the fix being on or off decides whether anything
    // is withheld, never whether anything is recorded.
    s.observing = true;

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
    return s && s->observing && !s->disabledForSession;
}

bool glitchFrameWantsBuffer(uint32_t bytes) {
    State* s = g_state;
    // Still true after the fix disables itself: the camera history is what a
    // user reports a flash with, and it cannot be recorded if nothing is
    // observed. The cost is one size compare per Map plus the read itself.
    return s && s->observing && bytes == s->bufferBytes;
}

void glitchFrameObserve(const void* data, uint32_t bytes, const void* resource) {
    State* s = g_state;
    // NOT gated on disabledForSession here. A stood-down fix still tracks the
    // camera so the history dump describes the moment the user pressed the key,
    // rather than the moment the fix gave up. The gate that matters -- never
    // acting -- is below, after the tracking.
    if (!s || !s->observing) return;
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

    // A head-space camera is not a reading. See kWorldCameraFloor2.
    //
    // Refused BEFORE it can become the frame's furthest, so a frame carrying
    // only head-space cameras ends with frameFarMag2 still negative -- which
    // every test below already treats as "nothing was seen this frame". It
    // therefore contributes no jump, no prediction update and no ring entry, and
    // the gap in the ring's frame numbers is what says it happened.
    if (m2 < kWorldCameraFloor2) return;

    if (m2 <= s->frameFarMag2) return;
    s->frameFarMag2 = m2;
    for (uint32_t a = 0; a < 3; ++a) s->frameFarPos[a] = pos[a];

    // LEARNED HERE, above every gate below it, and that placement is the point.
    //
    // A shell is evidence gathered by watching, so it must be gathered while the
    // detector is still validating, while it has stood down, and on frames with
    // no jump in them -- all states the returns below this line exit in. By the
    // time a swinging camera produces a mark the sphere is usually certified
    // already, and the mark never happens.
    const float radius = sqrtf(m2);
    observeShell(pos, radius);

    // Everything above is observation. Everything below can withhold a frame,
    // so a fix that has stood down -- or was never switched on -- stops here.
    if (!s->enabled || s->disabledForSession) return;

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

    // A RESIDUAL THAT IS NOT A NUMBER CANNOT BE JUDGED.
    //
    // finite3 above proves the POSITION is finite, and that is not enough: the
    // buffer at this offset stops holding a camera during heavy frames, and a
    // finite 1e26 squares to 1e52 against a float ceiling of 3.4e38. Measured in
    // a station arrival -- |pos| of 1.03e26 and 2.71e17 across six consecutive
    // frames at 2,200+ eye draws, in a ring the player dumped himself.
    //
    // Infinity then poisons the separation memory rather than merely being
    // wrong: fabsf(entry - inf) <= inf is TRUE, so an infinite residual matches
    // whichever entry it meets first and the running mean drags that entry to
    // infinity. A separation learned over a minute of flight is destroyed by one
    // frame of garbage, and the cascade it was suppressing starts costing frames
    // again with nothing in the log to connect the two.
    if (!std::isfinite(resid) || !std::isfinite(speed)) return;

    // Compared against a PREDICTION, not against the last position, so
    // travelling fast is not itself suspicious. The speed term then covers
    // acceleration: during a jump the camera legitimately gains thousands of
    // units per frame and a fixed floor alone would fire on all of it.
    const float trip = s->jumpMin > speed * s->jumpFactor ? s->jumpMin
                                                          : speed * s->jumpFactor;
    s->lastResid = resid;
    s->lastTrip = trip;

    const bool jumped = resid > trip;

    // A magnitude we have seen recur is a pass flip, not a transition.
    //
    // Checked HERE as well as at the boundary because this is the gate that
    // actually reaches the compositor -- the boundary runs at Present, after
    // Submit has already carried the flag to the headset. A suppression that
    // only happened at the boundary would withhold the frame and then tidy up
    // afterwards, which is the exact mistake the comment above this function
    // records being made once already.
    //
    // RE-DECIDED ON EVERY CANDIDATE, and there used to be an early return here
    // that skipped any candidate agreeing with the frame's current verdict. That
    // put the two halves out of step, because a frame carries SEVERAL cameras
    // and they are evaluated in increasing distance: the verdict was taken on
    // the first magnitude to cross the threshold, while `lastResid` -- the one
    // the boundary logs and remembers -- is the last one evaluated. A novel
    // magnitude opening the frame and a known one closing it therefore withheld
    // the frame and recorded it as recurring in the same breath.
    //
    // That is not a hypothesis. It is the pair of lines the field session
    // printed, on the same millisecond, for the same frame:
    //
    //   transition flash: a jump of about 564596 world units ... 2 times.
    //   transition flash: frame 13438 was drawn from 564654 world units ...
    //                     Withheld
    //
    // Both come from the withheld branch, in that order. Eleven consecutive
    // cascade frames were withheld while the memory holding their magnitude was
    // being refreshed by the very frames it should have been suppressing.
    // TWO INVARIANTS, ONE GATE. Geometry that repeats is a pass; a genuine
    // excursion is one frame and repeats nothing.
    //
    // The shell is tested on WHERE THE TRACK LANDED, not on how far it travelled
    // -- the whole reason it catches what the separation memory cannot is that
    // the destination is the invariant and the distance is not. Where it came
    // from is irrelevant.
    //
    // They are complementary rather than redundant, and the measured cases prove
    // it in both directions: 6v's block alternation holds one position, so it
    // has one direction and can never certify a sphere -- the separation memory
    // takes it. Today's swinging camera has a different jump size every time --
    // the shell takes it.
    s->parkSuppressedThisFrame = jumped && positionIsCertifiedPark(pos, radius);
    s->radiusSuppressedThisFrame =
        !s->parkSuppressedThisFrame && jumped && radiusIsCertifiedShell(radius);
    s->suppressedThisFrame = s->parkSuppressedThisFrame ||
                             s->radiusSuppressedThisFrame ||
                             (jumped && residualIsKnownSeparation(resid));
    s->jumpedThisFrame = jumped;
    if (jumped && !s->suppressedThisFrame && s->cooldown == 0 &&
        s->consecutive < s->maxConsecutive) {
        markGlitchFrame();
    } else {
        // Withdraw, do not clear: a further candidate later in the same frame
        // can re-raise this.
        unmarkGlitchFrame();
    }
}

void glitchFrameBoundary(uint32_t eyeDraws) {
    State* s = g_state;
    if (!s || !s->observing) return;

    ++s->frameNo;

    // OBSERVING BUT NOT ACTING. Record the frame and stop.
    //
    // Everything past here validates, decides and withholds, and none of it has
    // any business running for a fix the player has switched off. The ring does,
    // because a history is the whole reason to run with it off: nothing withheld
    // means anything seen was somebody else's.
    if (!s->enabled) {
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
        s->suppressedThisFrame = false;
        s->radiusSuppressedThisFrame = false;
        s->parkSuppressedThisFrame = false;
        return;
    }

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
        s->suppressedThisFrame = false;
        s->radiusSuppressedThisFrame = false;
        s->parkSuppressedThisFrame = false;
        return;
    }

    if (s->jumpedThisFrame && rendering && s->suppressedThisFrame) {
        // Recorded again, not merely counted: refreshing the entry is what stops
        // a separation that is suppressing correctly from ageing out of the
        // memory and firing all over again.
        ++s->suppressed;
        if (s->parkSuppressedThisFrame) {
            ++s->suppressedByPark;
            // Not recorded as a separation, for the same reason a radius
            // suppression is not: the jump size that landed here depends on
            // where it started, and the whole finding is that it varies.
        } else if (s->radiusSuppressedThisFrame) {
            ++s->suppressedByRadius;
            // NOT recorded as a separation. A radius-suppressed jump has a size
            // that varies with where the swing came from -- that is the reason
            // the separation memory misses these -- so feeding them in would
            // stock the table with magnitudes that mean nothing and widen its
            // chance of matching a real flash.
        } else {
            ++s->suppressedBySeparation;
            recordResidual(s->lastResid);
        }
        unmarkGlitchFrame();
    } else if (s->jumpedThisFrame && rendering && s->cooldown == 0 &&
        s->consecutive < s->maxConsecutive) {
        s->markedThisFrame = true;
        ++s->framesWithheld;
        ++s->windowWithheld;
        s->lastWithheldFrame = s->frameNo;
        // The first of a kind is always withheld -- it cannot be known to repeat
        // until it has. That is the cost of this approach and it is one frame per
        // novel magnitude, against one frame every three that it replaces.
        recordResidual(s->lastResid);
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
    // RECOGNISED GEOMETRY IS NOT A JUMP, SO IT MUST NOT REBASE EITHER.
    //
    // The three invariants suppress the MARK. They did not touch this, and this
    // is where the damage was: `awaitingReturn` keys on jumpedThisFrame, which
    // is set whether or not the jump was recognised. So every suppressed flip
    // still armed the return test, still failed it -- an auxiliary camera does
    // not come back to the view's path, it alternates -- and still bought a
    // 120-frame stand-down.
    //
    // On foot that is continuous. Measured, four transitions in one session: the
    // frame alternates between radius 5,009 and 53,713 every four to six frames,
    // so a 120-frame cooldown is re-armed roughly twenty times before it could
    // ever expire. The log carries 40 rebase notes, its whole budget, and zero
    // withheld frames.
    //
    // And in the middle of that, at f14962, one frame at radius 339,722 with
    // 5,009 and 53,713 either side of it -- a one-frame excursion that returns,
    // which is the shape the original bug was defined by (6t, 6u), at the exact
    // transition a player reported flashing. It was not suppressed: nothing
    // certified is within five times that radius. It was never judged at all,
    // because the detector had been standing down for the whole mode.
    //
    // So a suppressed frame now contributes NOTHING: no mark, no rebase, and no
    // entry in the view's track either. It is the same reasoning as the
    // world-camera floor -- a camera we have identified as an auxiliary pass is
    // not evidence about where the view is, in any direction.
    if (s->suppressedThisFrame) {
        // Deliberately empty. The track, the return test and the cooldown all
        // carry on from the last camera that WAS the view.
    } else if (s->awaitingReturn && s->markedThisFrame) {
        // STILL IN THE SAME GLITCH. This frame was judged off the path and
        // withheld on its own merits, so the event is not over and this is not
        // the moment to decide it was a change of reference frame. The run is
        // bounded by max_consecutive, which is the setting that exists to say
        // how long a glitch is allowed to be; the branch below ends it as soon
        // as a frame is NOT withheld, either because it came back or because the
        // run hit its cap.
    } else if (s->awaitingReturn && s->frameFarMag2 >= 0.0f) {
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
            // NOT recorded as a separation, deliberately, though it is a
            // repeating-magnitude memory sitting right here.
            //
            // `back` is measured against a two-step extrapolation of the
            // pre-jump path, so for a real change of reference frame it is the
            // size of that change -- and the one measured in the field was 4,999
            // units, which is squarely inside the range genuine transitions live
            // in (2,297 to 12,477 in the session that had both). Feeding it to
            // the suppressor would teach the detector to ignore a real flash of
            // that size later. The separations that matter here take the
            // RETURNED branch anyway: the cascade flips back, it does not stay.
            if (s->rebaseNotesLeft > 0) {
                --s->rebaseNotesLeft;
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
        // THE PREDICTION IS KEPT, and this is the change that makes a two-frame
        // glitch catchable at all.
        //
        // It used to be discarded here, which meant the frame after a withhold
        // could not be judged: glitchFrameObserve refuses to act below
        // camPrevValid 2. This file's own comment on transition_flash_max_
        // consecutive admitted the consequence -- "the cap is also unreachable
        // above 1 in practice: a marked frame resets the camera history, so the
        // next frame cannot be evaluated and a run of two never forms". A
        // documented setting that could never do anything, and a whole class of
        // glitch that could never be caught.
        //
        // The class is real and a player described it exactly: "below the planet
        // surface for a frame or two before being positioned correctly". One
        // frame withheld, the second shown, and the flash survives the fix.
        //
        // The bad frame does NOT enter the track -- camPrev and camPrev2 keep
        // their pre-jump values -- so the next frame is judged against the path
        // the view was on before the glitch, which is exactly the question worth
        // asking of it. The prediction is one frame stale by then, worth about
        // one frame of ordinary motion against a threshold in the thousands.
        //
        // The reason it was discarded is still real and is now handled by the
        // thing built for it: anchoring to a stale guess once withheld 44 frames
        // instead of 2. `consecutive` caps the run at max_consecutive, and the
        // rebase below ends it for good when the camera has genuinely moved.
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
    //
    // DO NOT TIGHTEN THIS LIMIT to catch pass flips. It was measured catching
    // them and that is not the good news it sounds like: 72 of 2000 on a planet
    // surface, which means twenty-five seconds of judder first and then no flash
    // fix at all for the rest of the session, from a scene that was rendering
    // perfectly correctly. Station arrivals legitimately burst, so a tighter
    // limit would take the fix off the field more often rather than less. The
    // separation memory above is what addresses that class; this stays a
    // backstop for a detector that is wrong in some way nobody has seen yet, and
    // after the suppression a trip here means something again.
    if (++s->windowFrames >= kRunawayWindow) {
        if (s->windowWithheld > kRunawayLimit) {
            s->disabledForSession = true;
            Log::get().note(
                "transition flash fix DISABLED for this session: %u of the last %u "
                "frames were withheld, far more than the handful of transitions any "
                "session contains. Whatever it is watching is not the camera, or this "
                "scene breaks the assumption it rests on. %u frame(s) had already been "
                "recognised as render-pass flips and left alone, so this is something "
                "else. The game renders normally from here; please report this with "
                "the log.",
                s->windowWithheld, s->windowFrames, s->suppressed);
        }
        s->windowFrames = 0;
        s->windowWithheld = 0;
    }

    // Running totals, on a timer rather than at shutdown. See kTotalsEvery.
    //
    // Gated on a counter having MOVED, so this says nothing at all through a
    // session where the fix never fires -- which is most of them, and which is
    // also the answer to "did it do anything": no line means no.
    if (s->frameNo - s->totalsAt >= kTotalsEvery) {
        s->totalsAt = s->frameNo;
        if (s->framesWithheld != s->totalsWithheld ||
            s->suppressed != s->totalsSuppressed) {
            const bool acted = glitchConsumerPresent();
            Log::get().note(
                "transition flash so far: %u frame(s) %s this session, and %u "
                "more recognised as render-pass geometry and left alone -- %u by a "
                "repeating jump size, %u by a camera orbiting at a fixed distance, "
                "%u by a camera parked in one place "
                "(%u and %u since the last of these lines). Compare the first "
                "number against how many jumps, drops and map closes you have "
                "made: roughly one per transition is it working. The others are "
                "expected to be large near a planet surface and are not a fault -- "
                "they are frames that would have been withheld before, and felt as "
                "judder.",
                s->framesWithheld, acted ? "withheld" : "detected but NOT withheld "
                                           "(openvr_api.dll is not installed)",
                s->suppressed, s->suppressedBySeparation, s->suppressedByRadius,
                s->suppressedByPark, s->framesWithheld - s->totalsWithheld,
                s->suppressed - s->totalsSuppressed);
            s->totalsWithheld = s->framesWithheld;
            s->totalsSuppressed = s->suppressed;
        }
    }

    if (s->markedThisFrame) ++s->consecutive; else s->consecutive = 0;
    if (s->cooldown > 0) --s->cooldown;
    s->markedThisFrame = false;
    s->jumpedThisFrame = false;
    s->suppressedThisFrame = false;
    s->radiusSuppressedThisFrame = false;
    s->parkSuppressedThisFrame = false;
    s->frameFarMag2 = -1.0f;
}

void dumpCameraRing(const char* trigger, uint32_t framesAfterPress) {
    State* s = g_state;
    if (!s) return;
    if (!s->observing) {
        Log::get().note("camera history dump requested, but nothing is being recorded: "
                        "the viewpoint offset and buffer size in [advanced] do not "
                        "describe a place a camera could be, so there was nothing to "
                        "watch.");
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

    // Where zero is, spelled out rather than left to be guessed.
    //
    // Two capture paths reach here and they have DIFFERENT zero points: the
    // immediate dump is written AT the press, the delayed one about two seconds
    // after it. Same columns, same units, and a reader who assumes the wrong one
    // is out by two seconds -- in the direction that hides the event, because it
    // pushes the moment being looked for off the end of the ring.
    char zero[160];
    if (framesAfterPress > 0) {
        snprintf(zero, sizeof(zero),
                 "%u frames AFTER the press -- roughly two seconds, so the press "
                 "itself is further back and what you reacted to further back "
                 "still",
                 framesAfterPress);
    } else {
        snprintf(zero, sizeof(zero), "the moment you pressed");
    }

    Log::get().note(
        "--- camera history: %llu frames, oldest first, written on %s. ZERO "
        "MILLISECONDS IS %s -- the reaction time between seeing something and "
        "reaching a key is what that column exists to let you subtract. Columns "
        "are milliseconds before that point, frame number, draws that reached the "
        "eye textures, and the camera position. A bad frame is one whose position "
        "leaves the line the frames either side of it are following, and returns. "
        "%u frame(s) withheld so far this session. ---",
        static_cast<unsigned long long>(have),
        trigger ? trigger : "the history key",
        zero, s->framesWithheld);

    for (uint64_t i = first; i < s->ringHead; ++i) {
        const RingEntry& e = s->ring[i % kRingFrames];
        const double msAgo =
            freq ? static_cast<double>(static_cast<int64_t>(newest - e.qpc)) * 1000.0 /
                       static_cast<double>(freq)
                 : 0.0;
        Log::get().note("CAM %8.1fms f%-7u eye=%-5u pos=(%+.2f %+.2f %+.2f)",
                        -msAgo, e.frame, e.eyeDraws, e.pos[0], e.pos[1], e.pos[2]);
    }
    // WAS ANY OF THIS OURS? The question every one of these dumps has been
    // opened to answer, worked out by hand every time.
    //
    // A player presses this key because they saw something. Whether EDVR
    // withheld a frame anywhere near that moment is the difference between "the
    // fix did this" and "the fix was not involved" -- and it took reading the
    // withheld list against the ring's frame numbers to find out. It is two
    // numbers we already have.
    if (s->framesWithheld == 0) {
        Log::get().note(
            "--- no frame has been withheld at all this session, so nothing in this "
            "history was EDVR withholding. Whatever was seen came from somewhere "
            "else. ---");
    } else {
        const uint32_t ago = s->frameNo - s->lastWithheldFrame;
        if (ago <= have) {
            Log::get().note(
                "--- the most recent withheld frame is %u, %u frame(s) before this "
                "dump, so it IS inside this history. %u withheld this session. ---",
                s->lastWithheldFrame, ago, s->framesWithheld);
        } else {
            Log::get().note(
                "--- NO frame was withheld within the %llu frames of this history. "
                "The most recent was frame %u, %u frames ago, and %u were withheld "
                "this session. Nothing in this capture was EDVR withholding. ---",
                static_cast<unsigned long long>(have), s->lastWithheldFrame, ago,
                s->framesWithheld);
        }
    }
    Log::get().note("--- end camera history ---");
}

// NOT REACHED ON A NORMAL GAME EXIT, and nothing that matters may live here.
//
// shutdown() calls this, and DllMain calls shutdown() only for a FreeLibrary
// unload -- which does not happen to a game's d3d11.dll. Process exit takes the
// other branch on purpose: every other thread is already dead and may have been
// holding the log's spinlock, so that path must not take one.
//
// So this runs for the test harness and for a hot unload, and that is all. The
// session totals it used to be the only source of are printed on a timer now
// (kTotalsEvery); if you find yourself wanting to add a summary here, it will
// not be read. Add it there instead.
uint32_t glitchFrameCertifiedShells() {
    State* s = g_state;
    if (!s) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < kShells; ++i) {
        if (s->shells[i].certified) ++n;
    }
    return n;
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
            "transition flash fix: %u frame(s) withheld this session, and %u more "
            "recognised as render-pass flips and left alone. Compare the first number "
            "against how many jumps, drops and map closes happened: roughly one per "
            "transition is it working, many more means it is firing on something else. "
            "The second number is expected to be large on a planet surface and is not "
            "a fault -- it is frames that would have been withheld before, and felt as "
            "judder. The openvr log counts the withheld ones from the other side and "
            "should read exactly twice the first number, once per eye.",
            s->framesWithheld, s->suppressed);
    }
    g_state = nullptr;
}

}  // namespace edvr
