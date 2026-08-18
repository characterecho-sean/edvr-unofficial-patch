#include "glitch_frame.h"

#include "../common/timing.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"

namespace edvr {
namespace {

// Frames of camera history held in memory and written out only when asked.
//
// SIZED, not timed. A ring is a buffer, so it has to be a count -- but the
// promise made about it everywhere ("the ten seconds before the fix gave up",
// "the ten seconds of history") is a duration, and 900 frames delivered that
// only at 90Hz: 12.5 seconds at 72 and 7.5 at 120. Sized for the fastest
// supported rate instead, so ten seconds is the FLOOR at every rate rather
// than the value at one of them. Nothing is written to disk unless the dump
// key is pressed, so the cost is one 40-byte struct per frame -- 48KB.
constexpr uint32_t kRingFrames = 1200;   // 10 s at 120Hz, 16.7 s at 72Hz

// How long to stand down for after deciding a jump was a change of reference
// frame rather than a glitch. This is a window during which nothing can be
// withheld -- a blind spot -- and the exposure it represents is wall-clock,
// not frames. Was 120 frames: 1.67 s at 72Hz, 1.0 s at 120.
constexpr uint64_t kRebaseCooldownMs = 1330;

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

// Evicted separation magnitudes remembered, so an insertion can be recognised
// as a RELEARN -- the table paying a withheld frame twice for one lesson.
//
// THE CHURN INSTRUMENT (spec §1g, EVIDENCE 6bp). The cull guard's wider
// frustum admits more near-surface passes and the recognition machinery
// churns with the margin: 29 recognitions at guard-off against 3,277 at half
// margin, 36 withheld. Whether that cost is this table THRASHING (more live
// pairs than sixteen slots, the failure the shell table had at eight) or
// genuine novelty (cascade geometry re-fitting continuously) decides between
// two completely different fixes -- capacity against a new invariant -- so
// the eviction memory exists to tell them apart in one field session, before
// either is built. It feeds counters and the ring dump. It changes nothing.
constexpr uint32_t kEvictedSeps = 32;

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
// Twenty seconds. This said, in its own comment, that it was copying vScreen's
// timer -- and then used 1800 frames, which is the constant vScreen converted
// to milliseconds precisely because it meant 25/20/15 seconds on the three
// supported headsets. Now it really is the same answer.
constexpr uint64_t kTotalsEveryMs = 20000;

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
// THE GAP WAS NOT EMPTY. Ten units was chosen because it "sits two decades
// above the head-space magnitudes it must exclude and nearly three below the
// smallest world camera ever recorded, in a gap four decades wide" -- and the
// arithmetic was right. The claim that nothing lives in the gap was not.
//
// Measured 2026-08-17, Pimax at 72Hz, twice in two sessions: a camera at
// (+6.05 +11.85 +2.50), magnitude 13.54, and the same one at 13.53 an hour
// earlier. Thirty-five per cent above the floor, so it read as a world
// camera. The previous world camera was at 4,214 units, so the composition
// of the two produced a phantom jump of 4,201 -- which is the figure in the
// log -- at the exact frame of a low-wake drop. Three frames were withheld
// on it (the max_consecutive cap), which at 72Hz is 42 ms of the previous
// image held over the moment of the drop. That IS the flash being reported:
// not a bad frame getting through, but good frames being withheld.
//
// It is pass composition, exactly as the paragraph above describes -- the
// jump is "the distance from the previous frame's world camera" to something
// that is not a world camera. The floor was simply a hair too low to catch
// this one.
//
// 250 is the log-midpoint of the two numbers that now bracket it: 13.54, the
// largest non-world camera ever measured, and 5,012, the smallest genuine
// world camera ever measured (6v records 9,967 and 136,405; the field has
// 5,012 / 7,600 / 568,000). Eighteen times above the first and twenty below
// the second. Head space at 0.09 is untouched either way.
//
// Widening this can only ever REDUCE what the detector judges, so the risk
// it carries is a genuine flash whose auxiliary camera sits between 10 and
// 250 units. Nothing measured in any session is in that band -- which was
// also true of 10 to 5,012 until this session, so the honest statement is
// that the band is empty as far as anything has looked.
constexpr float kWorldCameraFloor = 250.0f;
constexpr float kWorldCameraFloor2 = kWorldCameraFloor * kWorldCameraFloor;

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

// The let-through sample: how many lines a session, and how far apart.
//
// A SAMPLE, NOT A COUNT, and the log says so on every line. Every suppressed
// cascade frame is a jump that was let through -- 1,344 of them in one measured
// session -- so one line each would bury the log this exists to make readable.
// Forty lines spread at least 120 frames apart covers the transitions a player
// makes in a session without covering the steady-state noise between them, and
// the running totals line already carries the counts.
// The burst governor's shape. See the long note at the decision.
//
// Three withholds in sixty frames, then stand down for two seconds. Three is
// above what any single transition has ever cost (one or two) and far below the
// eight-in-fifteen storm that prompted this. The cooldown is long enough to sit
// out a storm and short enough that the next genuine event is still covered.
// The window stays in FRAMES because it is a user-facing ini value
// (advanced.transition_flash_burst_window, documented in frames with a
// 10-600 range); reinterpreting it as milliseconds would silently change
// every existing config. It is also the denominator of a rate, where the
// frame is a defensible unit. The COOLDOWN is internal and is a duration --
// "stand down for two seconds" -- so it becomes one.
constexpr uint32_t kDefaultBurstLimit = 3;
constexpr uint32_t kDefaultBurstWindow = 60;
constexpr uint64_t kBurstCooldownMs = 2000;
constexpr uint32_t kBurstHistory = 16;
// How often the stand-down may say so. Often enough that a governor which is
// down for most of a session is visible; rarely enough not to paper the log.
constexpr uint64_t kBurstNoteGapMs = 10000;

// The trust bar's shape, pinned by the three measured cases.
//
// Sixty seconds and three marks. The descent pairs marked 166 frames apart and
// the 568k cascade every 12.6 s -- both comfortably inside -- while the two low
// wakes were minutes apart and cannot accumulate. Three rather than two because
// two is what a pair of transitions produces.
//
// Every number in that paragraph is seconds, and the constant was 5400 frames:
// 45 seconds at 120Hz, which is only 3.6x the 12.6 s cascade cadence it has to
// contain rather than the 4.8x the sixty was chosen to give.
constexpr uint64_t kSepMarkWindowMs = 60000;
constexpr uint32_t kSepMarksToCertify = 3;

// Rule B's shape (1e step 3). A camera separating steadily from the view
// produces a strictly CLIMBING magnitude -- measured 5068 to 8769 across eight
// marks, each step 3.5 to 8.4 per cent above the last, so every one was novel
// to the separation memory's 2 per cent window and every one was withheld
// (6an, failure two: ~650 ms of stall in 170 ms of game time). The chain
// remembers the last withheld magnitude and where it landed; a new mark whose
// magnitude sits in the one-sided band just above the head AND whose landing
// is near the head's landing is the same camera one step further out.
//
// THE FIRST MARK ALWAYS MARKS. A chain exists only after a withhold has seeded
// it, so nothing here can deny the opening frame of a genuine flash -- the
// density guard lacked exactly this and blocked a real flash on its first
// field outing, which is why it is a stated requirement and not a preference.
//
// TWO BOUNDS BESIDE THE BAND, both flat and both measured, because the two
// shapes tried first were arguments and the fixtures refused each in turn.
// The spec sketched a ~600-frame TTL with a landing allowance proportional to
// the head: at 568k that allowance is 56,800 units and it swallowed the
// 11,928-unit gap between two WOBBLING CASCADE POSITIONS -- Rule B excusing
// the exact flips the separation memory exists to certify honestly. The
// second draft scaled the allowance with the chain's age: at age 22 that
// handed 5,500 units to the measured 5.7%-apart pair of GENUINE transitions,
// and at age 6 it swallowed the multi-candidate fixture's 912-unit landings.
//
// The measured storm (6an) is the only drift on record, and it is dense:
// marks 1-3 frames apart, landings stepping 220-428 units. So:
// - kDriftMaxGap = 8: the chain is dead past eight frames. 2-4x the measured
//   cadence, and refuses the 21-frame genuine pair, fixture O's 166-frame
//   descent pairs (the trust bar's case, not this rule's) and fixture L's
//   202-frame transition probe.
// - kDriftLandingUnits = 500: covers the measured 428-unit worst step with
//   margin, and is 24x smaller than the cascade wobble distance it must
//   refuse. Flat, because "the landing crawls" is the measurement; any growth
//   law on top of it was invention.
constexpr uint32_t kDriftMaxGap = 8;
constexpr float    kDriftLandingUnits = 500.0f;
constexpr float    kDefaultDriftPct = 10.0f;

constexpr uint32_t kLetThroughNotes = 40;
constexpr uint32_t kLetThroughGap = 120;

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

    // THE CULL GUARD'S STATE while this frame was drawn, packed as the
    // channel carries it (frame_flag.h), zero when the guard was off.
    //
    // Stamped so a staircase flight's dumps are self-describing: the margin
    // is live-tunable, so one capture can span guard-off and two margins, and
    // without the stamp nothing in the dump says which frames were which --
    // the exact attribution the 6bp churn measurement had to reconstruct
    // from log timestamps.
    uint32_t guard;

    // WHAT THE DETECTOR DECIDED ABOUT THIS FRAME, carried on the frame itself.
    //
    // The reason a jump was let through used to be reported only as a sampled
    // log line, capped at forty a session -- and a session near a planet
    // surface spends that cap in its first minute on repeating cascade
    // magnitudes. Measured 2026-08-15: a flash was captured in the ring at
    // f21941 and not one let-through line survived to describe it, because the
    // budget was gone long before. A sample cannot be relied on to cover the
    // one frame somebody pressed a key about.
    //
    // Stored per frame instead, and printed only when the ring is dumped: no
    // rate limit is needed because nothing is written until somebody asks, and
    // the answer is then guaranteed to be there for the frame they care about.
    uint8_t  verdict;
};

// What ended up in RingEntry::verdict. Order is the order the detector tests
// them, so the name matches the branch that produced it.
enum RingVerdict : uint8_t {
    kVerdictQuiet = 0,       // no jump this frame
    kVerdictWithheld,        // jumped, nothing excused it, frame withheld
    kVerdictPark,            // landed on a camera proven to sit in one place
    kVerdictShell,           // landed at a distance proven to be a render pass
    kVerdictSeparation,      // this jump size has happened before
    kVerdictCooldown,        // still settling after a recent jump
    kVerdictConsecutive,     // too many in a row already
    kVerdictBurst,           // the governor stood the fix down: see kDefaultBurstLimit
    kVerdictWithheldSepWould,// withheld, and the separation memory would have excused it
    kVerdictDrift,           // the drift chain's camera, one step further out (Rule B)
};

const char* ringVerdictName(uint8_t v) {
    switch (v) {
        case kVerdictWithheld:    return "WITHHELD";
        case kVerdictWithheldSepWould:
            return "WITHHELD -- the separation memory would have excused this";
        case kVerdictPark:        return "let through: a parked camera";
        case kVerdictShell:       return "let through: a known distance";
        case kVerdictSeparation:  return "let through: a repeating jump size";
        case kVerdictDrift:       return "let through: a separation drifting wider";
        case kVerdictCooldown:    return "let through: still settling";
        case kVerdictConsecutive: return "let through: too many in a row";
        case kVerdictBurst:       return "let through: the burst governor stood down";
        default:                  return "";
    }
}

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

    // The rebase stand-down, as a deadline. See kRebaseCooldownMs: this is a
    // window in which nothing can be withheld, and how long that blind spot
    // lasts is a wall-clock fact rather than a frame count.
    uint64_t cooldownUntilMs = 0;
    uint32_t consecutive = 0;

    uint32_t frameNo = 0;
    uint32_t renderedFrames = 0;   // frames that actually drew a scene
    // Eye draws of the PREVIOUS frame. The previous one because this frame's
    // count is still being accumulated while the detector runs, and because the
    // prediction is built from the previous frame's camera anyway.
    uint32_t lastEyeDraws = 0;
    uint32_t framesWithheld = 0;
    // Frames withheld during the draws that the boundary then judged not to be a
    // rendered scene. They WERE withheld -- the compositor had already acted --
    // so they are reported rather than deducted. A number that climbs here means
    // the eye-draw gate and the per-candidate decision disagree about what a
    // frame is, which is worth knowing and used to be invisible.
    uint32_t withheldNotRendering = 0;
    uint32_t notesLeft = 40;
    uint32_t rebaseNotesLeft = kRebaseNotes;

    // Jump magnitudes seen before. See kSeparations.
    struct Separation {
        float    resid = 0.0f;
        uint32_t lastSeen = 0;
        uint32_t hits = 0;
        bool     noted = false;

        // THE TRUST BAR. A magnitude is not believed because it happened twice.
        //
        // PAIRS CO-MOVE is the family's fourth clause and this is the member that
        // serves it: when both cameras track the moving view, positions drift so
        // no park certifies, radii change so no shell certifies, and the jump
        // between them is the only thing that holds still. Jump-keyed is correct
        // for relative geometry -- which is why this is the fourth invariant and
        // not a redundancy. What it lacked was a reason to be believed.
        //
        // Certification is by mark RECENCY-DENSITY, and the three measured cases
        // set the bar between them. The descent pairs marked 166 frames apart,
        // the 568k planetary cascade every 12.6 s, and the two low-wake
        // transitions minutes apart -- so a window that admits the first two and
        // refuses the third is what separates a co-moving pair from an event that
        // merely happens to repeat its size.
        uint32_t marks = 0;
        uint64_t lastMarkMs = 0;
        bool     certified = false;
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
    uint32_t   letThroughNotes = 0;
    // This frame's verdict, set at the decision and consumed at the boundary.
    uint8_t    verdictThisFrame = kVerdictQuiet;
    // The burst governor's book: frame numbers of recent withholds, and the
    // stand-down it triggers.
    uint32_t   withheldAt[kBurstHistory] = {};
    uint32_t   withheldHead = 0;
    uint32_t   burstLimit = kDefaultBurstLimit;
    uint32_t   burstWindow = kDefaultBurstWindow;
    uint64_t   burstStandDownUntilMs = 0;
    // What the separation memory is allowed to DO. 0 = off entirely, 1 = log
    // only (recognise and report, never excuse), 2 = act.
    //
    // Default is log-only, and that is a demotion made on evidence. The memory
    // is the only member of the family that is keyed on the JUMP rather than on
    // where the jump landed, and the only one that trusts a second sighting.
    // Both of its field failures came through exactly those two properties: it
    // learned a magnitude from a withheld low-wake transition and then used it
    // to excuse the next low-wake transition, because a transition to a fixed
    // reset position from a similar view radius repeats its jump size just as a
    // pass separation does. Transitions taught it to ignore transitions.
    //
    // Park and shell are keyed on the destination and certify by observation
    // over seconds, which transitions -- episodic, twice a session, minutes
    // apart -- can never satisfy. That asymmetry is why they are trusted to act
    // and this is not.
    uint32_t   separationMode = 1;
    uint64_t   burstNotedMs = 0;
    uint32_t   lastLetThroughFrame = 0;

    // --- the churn instrument (spec §1g): attribution, never decision ---
    // The channel reading for the frame being closed, taken once at the
    // boundary; everything below keys on it or feeds the ring dump.
    uint32_t   guardPacked = 0;
    bool       guardLiveNoted = false;
    uint32_t   withheldGuardLive = 0;
    uint32_t   suppressedGuardLive = 0;
    // The learning-cost counters: insertions are the tax being paid (each
    // novel magnitude's first mark was a withheld frame), live evictions are
    // knowledge lost while still current, relearns are the H1 number -- a
    // frame paid twice for one lesson, which only a too-small table produces.
    uint32_t   sepInsertions = 0;
    uint32_t   sepEvictedLive = 0;
    uint32_t   sepRelearned = 0;
    uint32_t   shellEvictedCertified = 0;
    struct EvictedSep {
        float    resid = 0.0f;
        uint32_t frame = 0;    // 0 = empty slot
    };
    EvictedSep evictedSeps[kEvictedSeps] = {};
    uint32_t   evictedHead = 0;

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
    uint32_t   suppressedByDrift = 0;
    bool       radiusSuppressedThisFrame = false;
    bool       parkSuppressedThisFrame = false;
    bool       driftSuppressedThisFrame = false;

    // Rule B: the drift chain. Seeded ONLY by an actual withhold, at the frame
    // boundary where wasWithheld is read -- so the first mark of anything has
    // already been paid before a chain exists. Advanced only by a frame this
    // rule itself suppressed; dead once nothing has advanced it for the TTL.
    float      driftHead = 0.0f;
    float      driftLanding[3] = {};
    uint32_t   driftFrame = 0;          // 0 = no chain
    float      driftPct = kDefaultDriftPct;
    // Periodic totals: when they were last printed, and what they said. Printed
    // only when a counter has moved, so a quiet session stays quiet.
    uint64_t   totalsAtMs = 0;
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

// The two stand-downs, asked as questions rather than read as counters.
//
// Both were `> 0` tests on per-frame countdowns. As deadlines they need a
// predicate, and having one named place for each is what kept the conversion
// honest: every site that used to decrement or compare now goes through these,
// so none of them can quietly disagree about what "standing down" means.
inline bool burstDown(const State* s) {
    return s->burstStandDownUntilMs != 0 && nowMs() < s->burstStandDownUntilMs;
}
inline bool rebaseDown(const State* s) {
    return s->cooldownUntilMs != 0 && nowMs() < s->cooldownUntilMs;
}

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
// WHY THERE IS NO DENSITY GUARD HERE, since one was added and taken out again.

// A cap of four withholds per ninety frames was added on 2026-08-15 to stop
// judder, on the strength of a totals line reporting 152 frames withheld in a
// session. That number was wrong: the compositor had withheld three. The
// counter re-derived its own verdict at the frame boundary instead of counting
// the decision, and the two had drifted -- see the note in glitchFrameBoundary.
//
// So the guard was built against judder this code was not producing, and it had
// a cost that showed up in the very next capture: frames f7708 and f7709 of the
// 14:51 session, a low-wake entry landing on the same reset position seen six
// times now, both let through with "too many in the last second". The guard
// blocked the withhold of the flash it was supposed to be helping with.
//
// The runaway guard already covers the case this was reaching for -- a detector
// firing constantly stands itself down for the session -- and it does so on
// evidence rather than on a rate. max_consecutive bounds a run. Nothing else is
// needed, and a second limiter whose premise was a measurement error is worse
// than nothing, because it fails exactly when the fix is most needed.
// KNOWN is not the same as TRUSTED, and this is the only place that decides.
//
// findSeparation answers "have we seen this magnitude"; this answers "has it
// earned the right to excuse a frame". The gap between those two questions is
// what let a low wake teach the memory to excuse the next low wake.
bool residualIsKnownSeparation(float resid) {
    const int i = findSeparation(resid);
    return i >= 0 && g_state->seps[i].certified;
}

// Has it been seen at all? For reporting what WOULD have been excused.
bool residualIsRecognisedSeparation(float resid) { return findSeparation(resid) >= 0; }

// Remember a magnitude, or refresh one already remembered.
//
// Refreshing matters as much as inserting. Without it a separation that is
// suppressing correctly ages out after a runaway window and fires again, which
// is the same judder at a longer period.
// A MARK counts toward certification. A refresh does not.
//
// recordResidual is called from two places: a frame that was WITHHELD, which is
// this magnitude costing the player something, and a frame that was suppressed,
// which is it costing nothing. Only the first is evidence that a co-moving pair
// is really there and really recurring, so only the first advances the bar.
void recordSeparationMark(float resid) {
    State* s = g_state;
    if (s->repeatPercent <= 0.0f || !std::isfinite(resid)) return;
    const int hit = findSeparation(resid);
    if (hit < 0) return;                       // recordResidual creates it
    State::Separation& e = s->seps[hit];
    // OUTSIDE THE WINDOW IS A RESTART, not an increment. Two low-wake
    // transitions minutes apart must never accumulate toward the same
    // certification -- that is the failure this whole bar exists to prevent.
    if (e.marks > 0 && !elapsedMs(e.lastMarkMs, kSepMarkWindowMs)) {
        ++e.marks;
    } else {
        e.marks = 1;
    }
    e.lastMarkMs = stampMs();
    if (!e.certified && e.marks >= kSepMarksToCertify) {
        e.certified = true;
        Log::get().note(
            "transition flash: a separation of about %.0f world units has cost a "
            "frame %u times within %u seconds, so it is a fixed gap between two "
            "render passes "
            "that MOVE WITH the view -- no fixed point to certify, no fixed "
            "radius either, which is why nothing else here catches it. Frames "
            "whose jump matches it are no longer withheld. A transition repeats "
            "its size too, but minutes apart, and cannot reach this.",
            static_cast<double>(e.resid), e.marks,
            (unsigned)(kSepMarkWindowMs / 1000));
    }
}

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
    // The churn instrument (see kEvictedSeps). Counted BEFORE the victim is
    // overwritten. An eviction only matters when the entry was still inside
    // the window -- evicting a stale one loses nothing, since findSeparation
    // had already stopped matching it. A same-frame false relearn cannot
    // happen: an in-window victim within tolerance of `resid` would have been
    // MATCHED above, and this path would never run.
    ++s->sepInsertions;
    if (s->seps[victim].hits > 0 &&
        s->frameNo - s->seps[victim].lastSeen <= kRunawayWindow) {
        ++s->sepEvictedLive;
        State::EvictedSep& ev = s->evictedSeps[s->evictedHead % kEvictedSeps];
        ev.resid = s->seps[victim].resid;
        ev.frame = s->frameNo;
        ++s->evictedHead;
    }
    {
        const float tol = resid * (s->repeatPercent * 0.01f);
        for (uint32_t i = 0; i < kEvictedSeps; ++i) {
            const State::EvictedSep& ev = s->evictedSeps[i];
            if (ev.frame != 0 && s->frameNo - ev.frame <= kRunawayWindow &&
                fabsf(ev.resid - resid) <= tol) {
                ++s->sepRelearned;
                break;
            }
        }
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

// Rule B: is this jump the drift chain's camera, one step further out?
//
// One-sided on purpose. At or below the head is the separation memory's
// territory -- a REPEATING size -- and above the band is a different event;
// the measured storm climbed 3.5 to 8.4 per cent per mark, and its one step
// past the band (6803 to 8769, +28.9%) is a step this must NOT excuse, which
// is what fixture M's "at most two marks" pins.
//
// THE LANDING TERM IS NOT OPTIONAL. A transition mid-drift can land at its
// reset position with a magnitude that happens to sit in the band -- fixture
// N's case -- and magnitude alone cannot tell it from the drifting camera.
// The drifting camera's landing CRAWLS; a transition lands kilometres away.
// See the note at kDriftLandingUnits for the two shapes this test refused
// before settling on the flat measured bound.
//
// AND DRIFT YIELDS TO RECOGNITION. A magnitude matching any separation entry
// -- certified or not -- is the REPEATING phenomenon, and it belongs to the
// separation memory whichever way that memory then rules: if this rule ate
// those frames the trust bar would starve (measured: fixture O's pair members
// sit 0.8% apart, and drift-suppressing the second member of every pair
// silently delayed certification -- frames suppressed here record no marks).
// The measured storm's steps were 3.5-8.4% apart and never matched within
// 2%, so drift loses nothing real by yielding.
bool residualIsDriftContinuation(float resid, const float* pos) {
    State* s = g_state;
    if (s->driftPct <= 0.0f) return false;
    if (s->driftFrame == 0) return false;
    const uint32_t age = s->frameNo - s->driftFrame;
    if (age == 0 || age > kDriftMaxGap) return false;
    if (!std::isfinite(resid)) return false;
    if (residualIsRecognisedSeparation(resid)) return false;
    const float head = s->driftHead;
    if (resid <= head) return false;
    if (resid > head * (1.0f + s->driftPct * 0.01f)) return false;
    return dist2(pos, s->driftLanding) <= kDriftLandingUnits * kDriftLandingUnits;
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
        // The churn instrument: a CERTIFIED entry evicted while still in the
        // window is the "learned and lost and learned again" failure this
        // table's own 8-to-64 history records, and whether 64 still thrashes
        // at large cull-guard margins is one of the questions the counter
        // answers. Uncertified evictions are the table working as intended.
        if (s->shells[victim].framesSeen > 0 &&
            s->frameNo - s->shells[victim].lastSeen <= kRunawayWindow &&
            (s->shells[victim].certified || s->shells[victim].parked)) {
            ++s->shellEvictedCertified;
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

// Which of the five it was, named the way the reader would ask.
//
// The order matters and mirrors the decision above exactly: park is tested
// first because it gates the shell, and the two counters are checked last
// because they apply only once nothing suppressed. Reading a different order
// here would produce a log that disagrees with the code it describes.
const char* letThroughReason(State* s, float resid) {
    if (s->parkSuppressedThisFrame)
        return "it landed on a camera already proven to sit in one place";
    if (s->radiusSuppressedThisFrame)
        return "it landed at a distance already proven to be a render pass";
    if (residualIsKnownSeparation(resid))
        return "a jump of this size has happened before, so it is a fixed gap "
               "between two render passes rather than the view moving";
    if (s->driftSuppressedThisFrame)
        return "it continued a separation already being watched drifting wider";
    if (burstDown(s))
        return "the burst governor is standing down after spending its withhold "
               "budget, so nothing is withheld until it comes back";
    if (rebaseDown(s))
        return "the detector is still settling after a recent jump and will not "
               "judge again yet";
    if (s->consecutive >= s->maxConsecutive)
        return "the frames before it were already withheld and withholding more "
               "in a row would be judder";
    return "nothing suppressed it and no counter blocked it, which should not be "
           "possible here -- please report this line";
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
    {
        const std::string mode =
            cfg.getString("advanced.transition_flash_separation", "act");
        s.separationMode = mode == "act" ? 2u : (mode == "off" ? 0u : 1u);
        if (mode != "act" && mode != "log" && mode != "off") {
            Log::get().note(
                "transition_flash_separation = '%s' is not act, log or off, so log "
                "is being used -- the repeating-jump-size rule reports what it "
                "would have excused without excusing it.",
                mode.c_str());
        }
    }
    s.burstLimit = static_cast<uint32_t>(cfg.getIntInRange(
        "advanced.transition_flash_burst_limit", kDefaultBurstLimit, 1, 30));
    s.burstWindow = static_cast<uint32_t>(cfg.getIntInRange(
        "advanced.transition_flash_burst_window", kDefaultBurstWindow, 10, 600));

    // THESE TWO ARE COUPLED, and nothing said so until it cost a flight.
    //
    // max_consecutive caps how long ONE run of withholds can be. burst_limit
    // is how many withholds inside burst_window spend the whole budget. If
    // the cap is not SMALLER than the budget, a single capped run spends it
    // by definition -- so every genuine transition, the thing the fix exists
    // for, is immediately followed by a stand-down in which nothing can be
    // withheld, and the rest of that same transition goes straight through.
    //
    // Measured 2026-08-17 on a Pimax at 72Hz, with max_consecutive at 3 and
    // the budget at 3: three excursions in one session, each exactly 3 frames
    // withheld, each followed by "the whole budget ... Standing down for 2000
    // ms". The third stand-down began 1.84 s before the player pressed the
    // history key, and a jump was let through 0.93 s before the press --
    // inside the blind window, and inside reaction time. The shipped default
    // of 2 against a budget of 3 does not do this; the player had the value
    // uncommented, and a stray keystroke ("3w", which strtol read as 3) is
    // what put it there.
    //
    // Said, not overridden. Both are deliberate escape hatches and a player
    // who wants this shape may have it -- but they should be told what it
    // costs rather than discovering it as a flash the fix was meant to hide.
    if (s.maxConsecutive >= s.burstLimit) {
        Log::get().note(
            "transition_flash_max_consecutive = %u is not below "
            "transition_flash_burst_limit = %u, so ONE run of withholds spends "
            "the whole burst budget and every transition is followed by a "
            "stand-down of %u ms in which nothing can be withheld -- including "
            "the rest of that transition. If you are seeing a flash on a wake "
            "drop or a landing, this is the first thing to change: leave "
            "max_consecutive at its default of 2, or raise burst_limit above "
            "it.",
            s.maxConsecutive, s.burstLimit, (unsigned)kBurstCooldownMs);
    }
    // A PERCENTAGE, like repeat_percent above and unlike the radius tolerance;
    // the ini says so beside it. 0 turns Rule B off -- the escape hatch if a
    // build ever produces genuine flashes that drift in lockstep with their
    // own landings, which nothing measured suggests but nothing rules out.
    const float driftPct = cfg.getFloat("advanced.transition_flash_drift_pct",
                                        kDefaultDriftPct);
    if (!std::isfinite(driftPct) || driftPct < 0.0f || driftPct > 50.0f) {
        Log::get().note(
            "transition_flash_drift_pct = %.1f is outside 0 to 50, so %.0f is "
            "being used. It is how far above the last withheld jump, in per cent, "
            "a new jump may sit -- landing nearby -- and still be the same camera "
            "drifting away rather than a fresh event. Too large and a real flash "
            "near a drift gets suppressed, which nothing would tell you.",
            static_cast<double>(driftPct), static_cast<double>(kDefaultDriftPct));
        s.driftPct = kDefaultDriftPct;
    } else {
        s.driftPct = driftPct;
    }
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
    // Rule B consults last: the certified invariants carry evidence gathered
    // over seconds, the chain carries one withhold's worth. The flag is stored
    // (not re-derived at the boundary) because the landing test needs THIS
    // candidate's position, which the boundary no longer has.
    s->driftSuppressedThisFrame =
        !s->parkSuppressedThisFrame && !s->radiusSuppressedThisFrame && jumped &&
        !(s->separationMode >= 2 && residualIsKnownSeparation(resid)) &&
        residualIsDriftContinuation(resid, pos);
    s->suppressedThisFrame = s->parkSuppressedThisFrame ||
                             s->radiusSuppressedThisFrame ||
                             s->driftSuppressedThisFrame ||
                             (jumped && s->separationMode >= 2 &&
                              residualIsKnownSeparation(resid));
    s->jumpedThisFrame = jumped;
    // WHY A JUMP WAS LET THROUGH, which the log has never said.
    //
    // It says why a frame WAS withheld, in detail. It has never said why one
    // was not, and there are five different reasons -- three suppressions and
    // two gates -- that are indistinguishable from outside. Measured cost of
    // that gap on 2026-08-15: a flash on leaving a low wake was captured in the
    // ring, cleared the threshold, and was not withheld; nothing in the log
    // could say which of the five let it go, so the leading explanation had to
    // be reconstructed by arithmetic on the ring afterwards and could not be
    // confirmed at all. An instrument that reports only its positives cannot
    // settle a report about a false negative.
    //
    // ONLY FOR JUMPS THAT CLEARED THE THRESHOLD, and rate-limited hard. Every
    // suppressed cascade frame is a jump -- 1,344 of them in one session -- and
    // a line each would bury the log it is meant to make readable.
    // THE BURST GOVERNOR: the fix may never cost more than the artefact.
    //
    // Not a rate limit on withholding -- a bound on the DAMAGE withholding does.
    // A withheld frame costs about 80 ms, because the compositor waits for a
    // submit that never comes. Measured 2026-08-15: eight withholds inside 170
    // ms of game time, alternating so max_consecutive never saw two in a row,
    // each followed by an 80 ms frame -- roughly 650 ms of stall spent hiding an
    // artefact that would have cost a fraction of that. The magnitudes were
    // 5068, 5492, 5796, 6008, 6225, 6500, 6803, 8769: one camera separating
    // steadily from the view, each step 3.5 to 8 per cent above the last, so the
    // 2 per cent match window never fired and every one was a novel magnitude.
    //
    // It is deliberately blind to WHY the storm is happening. Drift is one
    // cause and Rule B will handle that one properly; this bounds the cost of
    // every cause, including the ones no invariant models yet.
    //
    // HOW THIS DIFFERS FROM THE DENSITY GUARD THAT WAS REMOVED. That one denied
    // individual marks silently and, on its first field outing, denied the FIRST
    // mark of a genuine flash. This lets the budget's worth through untouched --
    // a real transition costs one or two withholds and never reaches three --
    // and only then stands the fix down, once, out loud. The residual risk is
    // real and worth naming: a storm immediately before a genuine flash can
    // still spend the budget, and then the flash is missed. That is the trade
    // the bound accepts, and the log says when it was taken so the miss is
    // attributable rather than mysterious.
    uint32_t recentWithholds = 0;
    for (uint32_t i = 0; i < kBurstHistory; ++i) {
        const uint32_t at = s->withheldAt[i];
        if (at != 0 && s->frameNo >= at && s->frameNo - at < s->burstWindow) {
            ++recentWithholds;
        }
    }
    if (!burstDown(s) && recentWithholds >= s->burstLimit) {
        s->burstStandDownUntilMs = nowMs() + kBurstCooldownMs;
        // Rate-limited, not once-only. It used to say this the first time and
        // stay silent for every later stand-down, so a governor that was down
        // most of a session looked like one that had fired once and recovered.
        if (dueMs(s->burstNotedMs, kBurstNoteGapMs)) {
            s->burstNotedMs = stampMs();
            Log::get().note(
                "transition flash: %u frames withheld inside %u -- the whole "
                "budget -- which costs about "
                "%u ms of stall, more than the flash being hidden. Standing down "
                "for %u ms. This is the fix refusing to be worse than the "
                "problem; a single transition costs one or two frames and never "
                "reaches this. If it keeps happening, something is producing a "
                "storm of jumps and the camera history will show what.",
                recentWithholds, s->burstWindow, recentWithholds * 80,
                (unsigned)kBurstCooldownMs);
        }
    }

    const bool willMark = jumped && !s->suppressedThisFrame && !rebaseDown(s) &&
                          s->consecutive < s->maxConsecutive &&
                          !burstDown(s);
    if (jumped && !willMark) {
        if (s->letThroughNotes < kLetThroughNotes &&
            s->frameNo - s->lastLetThroughFrame >= kLetThroughGap) {
            ++s->letThroughNotes;
            s->lastLetThroughFrame = s->frameNo;
            Log::get().note(
                "transition flash: frame %u jumped %.0f units (threshold %.0f) to "
                "(%+.0f %+.0f %+.0f) and was NOT withheld -- %s. This line is "
                "capped at %u a session and one per %u frames, so it is a sample, "
                "not a count.",
                s->frameNo, static_cast<double>(resid), static_cast<double>(trip),
                pos[0], pos[1], pos[2], letThroughReason(s, resid), kLetThroughNotes,
                kLetThroughGap);
        }
    }

    // The verdict, recorded on the frame rather than sampled into the log.
    // Same order as the tests above, so the name matches the branch.
    s->verdictThisFrame =
        !jumped                            ? kVerdictQuiet
        : willMark ? (s->separationMode >= 1 && residualIsRecognisedSeparation(resid)
                          ? kVerdictWithheldSepWould
                          : kVerdictWithheld)
        : s->parkSuppressedThisFrame       ? kVerdictPark
        : s->radiusSuppressedThisFrame     ? kVerdictShell
        : residualIsKnownSeparation(resid) ? kVerdictSeparation
        : s->driftSuppressedThisFrame      ? kVerdictDrift
        : burstDown(s)                     ? kVerdictBurst
        : rebaseDown(s)                    ? kVerdictCooldown
                                           : kVerdictConsecutive;
    if (willMark) {
        markGlitchFrame();
    } else {
        // Withdraw, do not clear: a further candidate later in the same frame
        // can re-raise this.
        unmarkGlitchFrame();
    }
#ifdef EDVR_GLITCH_TRACE
    // TEMPORARY investigation scaffolding, compiled only when the define is
    // passed by hand. Not part of any build script.
    if (jumped) {
        printf("[trace] f%u resid=%.0f trip=%.0f mark=%d park=%d shell=%d "
               "sepKnown=%d drift=%d burst=%u cd=%u consec=%u/%u head=%.0f "
               "chainAge=%u\n",
               s->frameNo, static_cast<double>(resid), static_cast<double>(trip),
               willMark ? 1 : 0, s->parkSuppressedThisFrame ? 1 : 0,
               s->radiusSuppressedThisFrame ? 1 : 0,
               residualIsKnownSeparation(resid) ? 1 : 0,
               s->driftSuppressedThisFrame ? 1 : 0, burstDown(s) ? 1u : 0u,
               rebaseDown(s) ? 1u : 0u, s->consecutive, s->maxConsecutive,
               static_cast<double>(s->driftHead),
               s->driftFrame ? s->frameNo - s->driftFrame : 0);
    }
#endif
}

void glitchFrameBoundary(uint32_t eyeDraws) {
    State* s = g_state;
    if (!s || !s->observing) return;

    ++s->frameNo;

    // The cull guard's state for the frame being closed, read once so the
    // stamp, the split counters and the log all describe the same reading.
    // The guard's stage transitions happen only at WaitGetPoses, before the
    // game queries its projections -- so the value standing at Present is the
    // value that governed everything this frame was drawn under. Zero is
    // "off" and also "nobody publishing", identical on purpose (frame_flag.h).
    s->guardPacked = cullGuardStatePacked();
    if (!s->guardLiveNoted && decodeCullGuardState(s->guardPacked).stage == 2) {
        s->guardLiveNoted = true;
        const CullGuardState g = decodeCullGuardState(s->guardPacked);
        Log::get().note(
            "transition flash: the cull guard's wider frustum is live "
            "(+%u.%u%% horizontal, +%u.%u%% vertical). A wider frustum admits "
            "more render passes, so recognition churn may climb here; "
            "camera-history lines now carry a cull= column and the running "
            "totals attribute their counts to the guard, so that churn is "
            "readable per margin. Bookkeeping only; no decision changes on "
            "it.",
            g.hPerMille / 10, g.hPerMille % 10, g.vPerMille / 10,
            g.vPerMille % 10);
    }

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
            e.guard = s->guardPacked;
            e.verdict = s->verdictThisFrame;
            for (uint32_t a = 0; a < 3; ++a) e.pos[a] = s->frameFarPos[a];
            ++s->ringHead;
        }
        s->frameFarMag2 = -1.0f;
        s->verdictThisFrame = kVerdictQuiet;
        s->jumpedThisFrame = false;
        s->suppressedThisFrame = false;
        s->radiusSuppressedThisFrame = false;
        s->parkSuppressedThisFrame = false;
        s->driftSuppressedThisFrame = false;
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
            e.guard = s->guardPacked;
            e.verdict = s->verdictThisFrame;
            for (uint32_t a = 0; a < 3; ++a) e.pos[a] = s->frameFarPos[a];
            ++s->ringHead;
        }
        s->frameFarMag2 = -1.0f;
        s->verdictThisFrame = kVerdictQuiet;
        s->jumpedThisFrame = false;
        s->suppressedThisFrame = false;
        s->radiusSuppressedThisFrame = false;
        s->parkSuppressedThisFrame = false;
        s->driftSuppressedThisFrame = false;
        return;
    }

    // ONE PREDICATE DECIDES AND COUNTS, and this used to be two.
    //
    // The decision was made per candidate during the draws -- that is the flag
    // the compositor reads at Submit -- and then the boundary RE-DERIVED it from
    // jumpedThisFrame, cooldown and consecutive to do the counting. Two
    // expressions for one thing, and they drifted: the boundary's copy never
    // learned about the density guard, and it reads consecutive as of the
    // boundary rather than as of the decision.
    //
    // Measured 2026-08-15: the d3d11 totals line claimed 152 frames withheld in a
    // session where the compositor withheld 6 eye-submits, which is 3 frames.
    // Fifty-fold, in the number a player is told to compare against how many
    // transitions they made -- and the number I used to diagnose judder, wrongly.
    //
    // verdictThisFrame IS the decision: set at the last candidate of the frame,
    // which is the state the flag was in when Submit read it. Counting it cannot
    // disagree with what happened.
    //
    // Rendering is deliberately NOT part of the test any more. It is known only
    // here, at the boundary, and the compositor has already acted by then -- so a
    // frame withheld during draws and later judged "not a rendered scene" WAS
    // withheld, and a counter that quietly drops it is lying in the same
    // direction as before. It is counted separately instead, below.
    const bool wasWithheld = s->verdictThisFrame == kVerdictWithheld ||
                             s->verdictThisFrame == kVerdictWithheldSepWould;
    if (s->verdictThisFrame != kVerdictQuiet && !wasWithheld) {
        // Recorded again, not merely counted: refreshing the entry is what stops
        // a separation that is suppressing correctly from ageing out of the
        // memory and firing all over again.
        ++s->suppressed;
        // Attribution, not decision: how much of the recognition traffic
        // arrives under the cull guard's wider frustum (spec §1g).
        if (decodeCullGuardState(s->guardPacked).stage == 2) {
            ++s->suppressedGuardLive;
        }
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
        } else if (s->driftSuppressedThisFrame) {
            ++s->suppressedByDrift;
            // The chain advances to the step it just excused -- the drifting
            // camera is HERE now, and the next step is judged from here. Once
            // per frame, at the boundary, like every other piece of withhold
            // bookkeeping; advancing at the decision would let several
            // candidates in one frame walk the chain forward together.
            //
            // NOT recorded as a separation, for the drift's own reason: these
            // magnitudes are each seen once by construction -- that is what
            // drifting means -- so the table would only accumulate junk with a
            // widening chance of matching a real flash.
            s->driftHead = s->lastResid;
            for (uint32_t a = 0; a < 3; ++a) s->driftLanding[a] = s->frameFarPos[a];
            s->driftFrame = s->frameNo;
        } else {
            ++s->suppressedBySeparation;
            recordResidual(s->lastResid);
        }
        unmarkGlitchFrame();
    } else if (wasWithheld) {
        // THE GOVERNOR IS CHARGED HERE, beside the counter, and not during the
        // draws where it used to be.
        //
        // A mark taken during the draws can still be withdrawn by a later
        // candidate in the same frame -- that is what unmarkGlitchFrame is for --
        // so charging at the decision billed the budget for frames that were
        // never withheld at all. Measured: a session with ZERO frames withheld
        // by either counter, in which the governor reported spending its whole
        // budget four separate times and let three flashes through.
        //
        // This is the third time one predicate has been evaluated in two places
        // and drifted: the withheld counter, then the burst budget twice. The
        // rule the file has arrived at is that anything derived from "was this
        // frame withheld" reads wasWithheld, here, once.
        s->withheldAt[s->withheldHead % kBurstHistory] = s->frameNo;
        ++s->withheldHead;
        // Counted, not excluded: see the note above on rendering.
        if (!rendering) ++s->withheldNotRendering;
        s->markedThisFrame = true;
        ++s->framesWithheld;
        ++s->windowWithheld;
        s->lastWithheldFrame = s->frameNo;
        if (decodeCullGuardState(s->guardPacked).stage == 2) {
            ++s->withheldGuardLive;
        }
        // The first of a kind is always withheld -- it cannot be known to repeat
        // until it has. That is the cost of this approach and it is one frame per
        // novel magnitude, against one frame every three that it replaces.
        recordResidual(s->lastResid);
        // Rule B's chain is seeded HERE, by the withhold itself -- which is the
        // structural form of "the first mark always marks": nothing can be
        // excused as a continuation until an actual withhold has established
        // what it would be continuing. A later novel withhold re-seeds, because
        // the freshest paid-for magnitude is the chain's base by definition.
        s->driftHead = s->lastResid;
        for (uint32_t a = 0; a < 3; ++a) s->driftLanding[a] = s->frameFarPos[a];
        s->driftFrame = s->frameNo;
        // AFTER recordResidual, which is what creates the entry. Counting first
        // meant the first occurrence of a magnitude never counted toward its own
        // certification, so the bar was silently four marks rather than three.
        recordSeparationMark(s->lastResid);
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
            s->cooldownUntilMs = nowMs() + kRebaseCooldownMs;
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
                    "rather than a bad frame. Standing down for %u ms instead of "
                    "withholding the rest of it.",
                    back, (unsigned)kRebaseCooldownMs);
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
        e.guard = s->guardPacked;
        e.verdict = s->verdictThisFrame;
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
    if (dueMs(s->totalsAtMs, kTotalsEveryMs)) {
        s->totalsAtMs = stampMs();
        if (s->framesWithheld != s->totalsWithheld ||
            s->suppressed != s->totalsSuppressed) {
            const bool acted = glitchConsumerPresent();
            // Present only in sessions the guard has been live in, so every
            // other rig's totals line reads exactly as it always has.
            char guardSplit[176] = "";
            if (s->withheldGuardLive || s->suppressedGuardLive) {
                snprintf(guardSplit, sizeof(guardSplit),
                         " Of those, %u withheld and %u recognised happened "
                         "while the cull guard's wider frustum was live -- the "
                         "margin admits more render passes, and this is that "
                         "cost being counted (docs/terrain-culling.md).",
                         s->withheldGuardLive, s->suppressedGuardLive);
            }
            Log::get().note(
                "transition flash so far: %u frame(s) %s this session, and %u "
                "more recognised as render-pass geometry and left alone -- %u by a "
                "repeating jump size, %u by a camera orbiting at a fixed distance, "
                "%u by a camera parked in one place, %u by a separation drifting "
                "wider "
                "(%u and %u since the last of these lines; %u were withheld on a "
                "frame the eye-draw gate did not call a rendered scene). Compare "
                "the first "
                "number against how many jumps, drops and map closes you have "
                "made: roughly one per transition is it working. The others are "
                "expected to be large near a planet surface and are not a fault -- "
                "they are frames that would have been withheld before, and felt as "
                "judder.%s",
                s->framesWithheld, acted ? "withheld" : "detected but NOT withheld "
                                           "(openvr_api.dll is not installed)",
                s->suppressed, s->suppressedBySeparation, s->suppressedByRadius,
                s->suppressedByPark, s->suppressedByDrift,
                s->framesWithheld - s->totalsWithheld,
                s->suppressed - s->totalsSuppressed, s->withheldNotRendering,
                guardSplit);
            s->totalsWithheld = s->framesWithheld;
            s->totalsSuppressed = s->suppressed;
        }
    }

    if (s->markedThisFrame) ++s->consecutive; else s->consecutive = 0;
    s->markedThisFrame = false;
    s->jumpedThisFrame = false;
    s->suppressedThisFrame = false;
    s->radiusSuppressedThisFrame = false;
    s->parkSuppressedThisFrame = false;
    s->driftSuppressedThisFrame = false;
    s->frameFarMag2 = -1.0f;
    s->verdictThisFrame = kVerdictQuiet;
}

void dumpCameraRing(const char* trigger, uint32_t msAfterPress) {
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
    // In MILLISECONDS since the delay became one. It was frames, and passing
    // the converted delay straight in printed "2 frames AFTER the press" for
    // what is two seconds -- the exact off-by-two-seconds this paragraph
    // exists to prevent, introduced by the change meant to make it right.
    //
    // Two capture paths reach here and they have DIFFERENT zero points: the
    // immediate dump is written AT the press, the delayed one about two seconds
    // after it. Same columns, same units, and a reader who assumes the wrong one
    // is out by two seconds -- in the direction that hides the event, because it
    // pushes the moment being looked for off the end of the ring.
    char zero[160];
    if (msAfterPress > 0) {
        snprintf(zero, sizeof(zero),
                 "%u ms AFTER the press, so the press itself is further back "
                 "and what you reacted to further back still",
                 msAfterPress);
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
        // The cull guard's margin, only on frames it was doing something --
        // a guard-off session's dump is byte-identical to what it always
        // was, and a staircase flight's dump names the margin per frame,
        // across live changes, which is the attribution 6bp had to
        // reconstruct from log timestamps.
        char cull[28] = "";
        if (e.guard) {
            const CullGuardState g = decodeCullGuardState(e.guard);
            if (g.stage == 1) {
                snprintf(cull, sizeof(cull), " cull=stage1");
            } else {
                snprintf(cull, sizeof(cull), " cull=+%u.%u%%/+%u.%u%%",
                         g.hPerMille / 10, g.hPerMille % 10, g.vPerMille / 10,
                         g.vPerMille % 10);
            }
        }
        Log::get().note("CAM %8.1fms f%-7u eye=%-5u pos=(%+.2f %+.2f %+.2f)%s %s",
                        -msAgo, e.frame, e.eyeDraws, e.pos[0], e.pos[1], e.pos[2],
                        cull, ringVerdictName(e.verdict));
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
    // WHAT THE DETECTOR HAS LEARNED, printed beside the history it learned it
    // from (spec §1g). The note lines are a sample and the totals are counts;
    // this is the CONTENTS -- which magnitudes and which landing geometry are
    // doing the suppressing, and what the learning has cost. On a cull-guard
    // staircase flight this is the per-step readout: dump at each margin and
    // the tables name what that margin taught, which is the data the
    // margin-aware-detector decision waits on.
    {
        uint32_t sepsInUse = 0;
        for (uint32_t i = 0; i < kSeparations; ++i) {
            if (s->seps[i].hits > 0) ++sepsInUse;
        }
        if (sepsInUse > 0) {
            Log::get().note(
                "--- learned separations: %u of %u slots in use. A separation "
                "is a repeating jump size, the fixed gap between two render "
                "passes; CERTIFIED means it has cost %u frames inside %u s and "
                "may excuse matches. ---",
                sepsInUse, kSeparations, kSepMarksToCertify,
                (unsigned)(kSepMarkWindowMs / 1000));
            for (uint32_t i = 0; i < kSeparations; ++i) {
                const State::Separation& e = s->seps[i];
                if (e.hits == 0) continue;
                Log::get().note(
                    "SEP ~%-9.0f hits=%-6u marks=%u%s  last seen %u frame(s) ago",
                    static_cast<double>(e.resid), e.hits, e.marks,
                    e.certified ? " CERTIFIED" : "",
                    s->frameNo - e.lastSeen);
            }
        }
        uint32_t shellsInUse = 0;
        for (uint32_t i = 0; i < kShells; ++i) {
            if (s->shells[i].framesSeen > 0) ++shellsInUse;
        }
        if (shellsInUse > 0) {
            Log::get().note(
                "--- landing geometry: %u of %u slots in use. ORBIT = one "
                "distance, many bearings, never resting; PARK = one place, "
                "never moving; DISQUALIFIED = the view itself was shown to "
                "live there. ---",
                shellsInUse, kShells);
            for (uint32_t i = 0; i < kShells; ++i) {
                const State::Shell& e = s->shells[i];
                if (e.framesSeen == 0) continue;
                if (e.parked) {
                    Log::get().note(
                        "SHL r=%-8.0f PARK at (%+.0f %+.0f %+.0f)  seen=%-5u "
                        "last %u frame(s) ago",
                        static_cast<double>(e.radius), e.certParkPos[0],
                        e.certParkPos[1], e.certParkPos[2], e.framesSeen,
                        s->frameNo - e.lastSeen);
                } else {
                    Log::get().note(
                        "SHL r=%-8.0f %-12s seen=%-5u dirs=%-3u maxDwell=%-4u "
                        "last %u frame(s) ago",
                        static_cast<double>(e.radius),
                        e.certified          ? "ORBIT"
                        : e.dwellDisqualified ? "DISQUALIFIED"
                                              : "gathering",
                        e.framesSeen, e.distinctDirs, e.maxDwell,
                        s->frameNo - e.lastSeen);
                }
            }
        }
        if (s->sepInsertions || s->shellEvictedCertified) {
            Log::get().note(
                "--- learning cost so far: %u separations learned (each novel "
                "one's first mark was a withheld frame), %u evicted while "
                "still current, %u RELEARNED after eviction (a frame paid "
                "twice for one lesson -- the number that says the table is "
                "too small), %u certified orbits/parks evicted while current. "
                "---",
                s->sepInsertions, s->sepEvictedLive, s->sepRelearned,
                s->shellEvictedCertified);
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

GlitchFrameChurnStats glitchFrameChurnStats() {
    GlitchFrameChurnStats out = {};
    State* s = g_state;
    if (!s) return out;
    out.withheldGuardLive = s->withheldGuardLive;
    out.suppressedGuardLive = s->suppressedGuardLive;
    out.sepInsertions = s->sepInsertions;
    out.sepEvictedLive = s->sepEvictedLive;
    out.sepRelearned = s->sepRelearned;
    out.shellEvictedCertified = s->shellEvictedCertified;
    return out;
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
