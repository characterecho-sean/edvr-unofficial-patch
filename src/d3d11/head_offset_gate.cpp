#include "head_offset_gate.h"

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"
#include "../common/timing.h"

namespace edvr {
namespace {

// EVERY THRESHOLD BELOW IS A DURATION, so it is milliseconds. See timing.h for
// why: these were frame counts, and each one silently meant something different
// at 72Hz, 90Hz and 120Hz -- the three rates this mod has to work at. The
// counters themselves stay in frames where a frame is the natural unit (a
// one-frame panel dropout is a hitch whatever the rate); only the tests that
// ask "how long" read the clock.

// Vehicle-scene TIME after which the panel's return means a NEW on-foot
// session rather than the same one continuing. Ten seconds: the shortest ship
// leg between landings is minutes and a boarding animation alone is tens of
// seconds, while every same-session interruption is either shorter or is not
// vehicle scene at all -- a camera stint is in-camera (excluded by the
// counter's own condition) and a map or menu is idle, not scene. Measured
// need: at every observed second landing the game had reset its camera view to
// 0 while EDVR held or counted the old one (6ay), so the count must restart at
// the game's own reset point to stay anchored.
constexpr uint64_t kNewFootSessionMs = 10000;

// How long the journal's Disembark may stand in for a stale Status.json.
// Measured flag lag after the event: ~5.8 s (session 1, 10:57:06->10:57:12)
// and <=6.7 s (session 2, 11:02:21->11:02:28), so ten seconds covers the
// airlock animation with margin while staying far below a real ship leg.
// The grace usually ends earlier anyway -- at the first on-foot sample.
//
// This is the one that most needed a clock. As 900 frames it was 7.5 seconds
// at 120Hz, against a measured worst case of 6.7 -- eight hundred milliseconds
// of margin on a number whose whole justification was "with margin". A player
// on a 120Hz headset and a slightly slower airlock lost the entry outright.
constexpr uint64_t kFootGraceMs = 10000;

// The KEYLESS entry window. The keyed window (head_offset_enter_window) is
// paced by a keypress, which is instant; the keyless confirmation is paced by
// Status.json -- the poll interval plus the game's own ~1 Hz write cadence.
// Measured arrivals after the panel stopped: +53 frames (11:00:34, latched)
// and +90 frames (11:27:10, forfeited by the then 60-frame window while the
// player stood in the camera with the read alive on view 2). Both measurements
// are at 90Hz, so ~590ms and ~1000ms. 2.7 seconds covers poll + write with
// margin at any rate. The boarding hazard this window brushes is bounded the
// same way it always was: a wrong latch dies at the next not-on-foot sample,
// one poll later.
constexpr uint64_t kKeylessEnterMs = 2700;

// Neither panel nor scene for this long: a menu, a loading screen, or a mode
// change we cannot see. Was 300 frames, which a 1790fps loading screen passed
// in 170ms -- the exact case it was written to sit through.
constexpr uint64_t kIdleDropMs = 3300;

// A panel run that has been gone this long is over, rather than interrupted.
// Was 90 frames, described as "still generous for a stutter": a claim about
// milliseconds, and 750ms of it at 120Hz.
//
// BRACKETED FROM BOTH SIDES BY MEASUREMENT, and the bracket is narrow enough
// that the old 90 frames only fitted at 90Hz:
//   - it must be LONGER than 1000 ms, because the keyless path waits on
//     Status.json and the slowest measured sample landed 1000 ms after the
//     panel stopped (11:27:10). On-foot credit dying first makes the keyless
//     entry window unusable however wide that window is;
//   - it must be SHORTER than about 1333 ms, because the same credit is what
//     let boarding a ship and pressing the camera key "within about three
//     seconds" arm the ON-FOOT offset in a cockpit -- the outcome this gate
//     exists to prevent, and the one the ship-vanity fixture pins.
// 1150 sits in the middle of that. At 90Hz the old value was 1000 ms, which
// is not inside the bracket but exactly on its floor: the keyless fixture
// passed by nine hundredths of a millisecond of accumulated rounding.
constexpr uint64_t kPanelRunOverMs = 1150;

// Draws into the eye textures in one frame, above which the world is being
// rendered in stereo rather than composited from the flat panel. Lower than
// vScreen's kSceneEyeDraws, and deliberately: that one separates gameplay
// from a MENU across a whole session, while this one has to be cleared by a
// single frame of a camera the player has just entered.
//
// Named rather than written three times. The rejection line below quotes it
// back to the reader, and a message that disagrees with the test it describes
// is worse than no message -- this is the same drift class as the ini
// defaults that did not match the code's.
constexpr uint32_t kSceneDraws = 50;

// How often the in-camera heartbeat may repeat. Diagnostics only.
constexpr uint64_t kHeartbeatMs = 6700;

// One instance, file-local. The gate is per-process by nature -- there is one
// player in one mode -- and keeping it out of the render hooks' State is what
// lets the same logic serve both builds instead of being forked into each.
struct Gate {
    bool     gateWantsPanel = false;     // config: is the gate switched on
    uint32_t gatePanelRun = 0;           // consecutive frames settled on the panel
    uint32_t gateSincePanel = 0;         // frames since the panel was last seen
    uint32_t gateIdleFrames = 0;         // frames with neither panel nor scene
    // Scene-without-panel frames OUTSIDE the camera since the panel was last
    // seen: ship or SRV time. The panel returning after enough of this is a
    // NEW on-foot session. The game does NOT reset its camera view across
    // that boundary -- see the reset at the panelNow block.
    uint32_t gateAwayScene = 0;
    bool     gateExternal = false;       // the latch itself
    uint32_t gateSinceEnter = 0;         // frames since the latch was set
    uint64_t gateEnterWindowMs = 670;    // after the panel stops
    uint32_t gateNearMisses = 0;         // rejected candidates, logged
    // The busiest frame of the CURRENT panel-less gap.
    //
    // The arm test wants ONE frame above the scene threshold, so the number
    // that decides it is the gap's peak, not whatever the last frame happened
    // to carry. Reported by the rejection line below: "it never got close" and
    // "it got there and something else failed" are different bugs, and a
    // single frame's value cannot tell them apart.
    uint32_t gapEyeDraws = 0;
    bool     gateIntent = false;
    bool     gateHaveKey = false;      // a press has been seen
    bool     gateKeyBound = false;     // a key is CONFIGURED, pressed or not
    int32_t  gateViewIndex = 0;
    int32_t  gateWantView = 1;           // -1 = any view; matches edvr.ini
    int32_t  gateViewCount = 6;          // 0 = do not wrap; matches edvr.ini
    bool     gateHaveNextKey = false;
    bool     gateViewWarned = false;
    bool     gatePanelSeenNoted = false;
    bool     gateViewSynced = false;

    // THE DESYNC INSTRUMENT (advanced.camera_view_desync_log).
    //
    // The sync line above is said once a session and then the count is
    // corrected in silence, which is right for a user and useless for the
    // question "WHEN does the game reset its own index". Armed, this logs
    // every distinct divergence with the context that names the boundary.
    // Off by default; it needs the memory read on, which is not the shipped
    // state either.
    // The ring the counted view is walking right now, and the one it was
    // walking when we last looked. A change between them is a context
    // transition, and the index folds across it exactly as the game's does.
    int32_t  gateViewCountSrv = 8;
    int32_t  gateViewCountShip = 11;
    int      gateVehicle = 0;          // matches JournalVehicle::Unknown
    // The wake edge. `wakeKnown` false means the status file has not answered
    // yet, and an unknown-to-true transition is not an edge, only the first
    // thing we happened to see.
    bool     wakeKnown = false;
    bool     wakeLast = false;
    bool     desyncLog = false;
    int32_t  lastDesyncGame = -2;
    int32_t  lastDesyncCount = -2;
    bool     gateViewEverRead = false;   // something has supplied a real index
    bool     gateViewLostNoted = false;
    // The view bridge: a read that has died is covered by the counted view
    // for as long as it takes to come back. No expiry -- the player stays in
    // Explorer Cam as long as they wish (stated as a product requirement,
    // 2026-08-15), and the held value cannot go stale outside the camera
    // because the game freezes it there. See the long note at the loss
    // decision for the exposure this accepts.
    bool     gateBridgeOn = true;        // fix.head_offset_view_bridge
    bool     gateBridgeStarted = false;  // once per contiguous unreadable run
    bool     gateSyncRefusedNoted = false;
    uint32_t gateFrameNo = 0;            // the frame Frame() last ran for

    // STAMPS FOR THE DURATION TESTS, each paired with the frame counter above
    // it rather than replacing it. Both are wanted: the counters answer "how
    // many frames" for the debounces and for the log lines, which are read
    // against frame-indexed fixtures, and these answer "how long" for the
    // thresholds that were durations all along. A stamp of 0 means the run it
    // measures is not in progress, which elapsedMs() reads as "not yet".
    uint64_t panelStoppedMs = 0;         // when gateSincePanel started counting
    uint64_t awaySceneMs = 0;            // when gateAwayScene started counting
    uint64_t idleMs = 0;                 // when gateIdleFrames started counting
    uint64_t lastFootResetMs = 0;        // when a new-session reset last fired
    uint64_t heartbeatMs = 0;            // when the in-camera heartbeat last spoke
    // The game's live on-foot word (Status.json via the journal watcher).
    bool     liveOnFootKnown = false;
    bool     liveOnFoot = false;
    bool     autoIntentNoted = false;
    uint32_t liveSample = 0;             // running Status.json sample count
    uint32_t sampleAtPanelStop = 0;      // liveSample when the panel stopped
    // THE DISEMBARK'S STALE-STATUS WINDOW (measured 2026-08-16, both field
    // sessions): after the journal's Disembark, Status.json keeps answering
    // "not on foot" for ~6 seconds of airlock animation. Until the flag has
    // been observed TRUE in this foot session, false describes the PREVIOUS
    // leg -- so the boarding-exit must not fire on it (it killed the latch
    // six frames running at 10:57:12), and keyless arming leans on the
    // journal's own declaration instead of losing entries to the window.
    bool     liveOnFootSeenThisFoot = false;
    bool     footGraceJournal = false;   // Disembark declared, flag not yet true
    uint32_t footGraceFrame = 0;         // when that declaration landed
    uint64_t footGraceMs = 0;            // ...and the same, on the clock
    bool     gateKeylessOn = false;      // experimental.keyless_camera (parked)
    bool     gateNoConsumerNoted = false;
    uint32_t gateIntentAge = 0;          // frames since the key was pressed
    uint64_t gateIntentMs = 0;           // ...and when, on the clock
    uint64_t gateIntentGraceMs = 2000;   // how long a press gets to take effect
    bool     gateInCamera = false;       // in the camera, whatever the view
    uint32_t gateEnters = 0, gateExits = 0;
    // Camera entries (the gateInCamera latch), distinct from gateEnters which
    // counts the OFFSET arming: the scan nudge wants the camera edge, before
    // the player has cycled to the right view.
    uint32_t gateCameraEnters = 0;

    // Set by headOffsetGateSetView. -1 means nobody can tell us, so the
    // keypress count stands.
    int      viewOverride = -1;
    // The flat panel has been composited steadily. A weak signal on purpose --
    // see the header.
    bool     panelSettled = false;
};

Gate g;

}  // namespace

void headOffsetGateConfigure() {
    Config& cfg = Config::get();
    // Read first and unconditionally: an instrument that only arms on some
    // paths through this function is an instrument that lies about when it
    // was watching.
    g.desyncLog = cfg.getBool("advanced.camera_view_desync_log", false);
    // DEFAULTS HERE MATCH edvr.ini's DEFAULTS, and they did not.
    //
    // head_offset_view defaulted to -1 in code and 1 in the file; view_count to
    // 0 and 6. A user with a partial or pasted ini therefore got "any view",
    // which arms the offset in the front-facing camera as well -- the opposite
    // of what the file they were reading said. This is the same class as the
    // commented-out log.max_mb that migrate_ini found, on the two keys with the
    // highest consequence in the feature.
    //
    // BOUNDED, too. Every one of these was cast straight to uint32_t, so a
    // negative became about 4.29e9: an intent grace of thirteen hours, an entry
    // window that never closes, a ceiling that never fires. Four settings, one
    // silent failure shape -- the feature behaves as though the setting were
    // absent, which is the hardest thing to attribute from a log.
    // The gate costs a GetDesc per eye-sized draw, so it is only paid for when
    // something wants the answer. Default ON: the head offset it feeds is the
    // feature, and an ungated offset moves the cockpit view.
    g.gateWantsPanel = cfg.getBool("fix.head_offset_gate", true);
    // 60, not 5.
    //
    // 5 came from two entries that both logged "2 frames", and a sample of two
    // is not a distribution: the third measured SIX and was rejected by one
    // frame, so the offset never armed at all.
    //
    // More to the point, the tight window was a PROXY for intent. It existed to
    // reject boarding the ship and leaving Cinema Mode, which look identical to
    // entering the camera in render state alone. hotkey.external_camera now
    // supplies intent directly, and a keypress is not a heuristic -- so the
    // window goes back to being what it should always have been: a sanity bound
    // on "was the panel recently on screen", not the discriminator.
    //
    // Without a key bound it is still the only thing standing between the gate
    // and every other way of leaving the panel, which is why it is not simply
    // removed.
    // MILLISECONDS, AND RENAMED TO SAY SO. These two are the SHIPPED entry
    // path -- the keyless one is parked -- so leaving them in frames would
    // have left the default route rate-dependent while the parked one was
    // fixed. The names carry _ms rather than the values being reinterpreted,
    // because a user who had written 60 into the old key meant 60 frames and
    // silently reading it as 60 ms would give them a twentieth of the window
    // they asked for. An old key is simply not read; the new one defaults.
    g.gateEnterWindowMs = static_cast<uint64_t>(
        cfg.getIntInRange("fix.head_offset_enter_window_ms", 670, 0, 100000));
    g.gateIntentGraceMs = static_cast<uint64_t>(
        cfg.getIntInRange("fix.head_offset_intent_grace_ms", 2000, 0, 100000));
    g.gateWantView = cfg.getIntInRange("advanced.head_offset_view", 2, -1, 63);
    g.gateViewCount = cfg.getIntInRange("fix.head_offset_view_count", 6, 0, 64);
    g.gateViewCountSrv =
        cfg.getIntInRange("fix.head_offset_view_count_srv", 8, 0, 64);
    g.gateViewCountShip =
        cfg.getIntInRange("fix.head_offset_view_count_ship", 11, 0, 64);
    g.gateBridgeOn = cfg.getBool("fix.head_offset_view_bridge", true);
    // KEYLESS ENTRY IS PARKED, default off (product decision 2026-08-16).
    // The entry side field-certified -- grace, window, boarding-exit all
    // landed -- but the VIEW cannot be supplied without presses when the
    // game runs its near-settlement copy circus: 6bf measured a stale array
    // at the usual base reading 0 for an entire camera stay while the
    // player cycled the real one, and certification honestly refused all
    // the way down. Keyed mode counts presses and is immune. The machinery
    // stays for the controller-support work, behind this flag.
    g.gateKeylessOn = cfg.getBool("experimental.keyless_camera", false);
    // The "you have not bound the view key" warning does NOT live here.
    //
    // It did, and it was wrong the moment a build could read the view from
    // somewhere else: config time is too early to know whether anything will
    // supply an index, so it announced that the offset could never arm to a
    // build where it demonstrably does. It has moved into the frame path,
    // which can tell. See headOffsetGateFrame.
}

void headOffsetGateReset() {
    const bool wasOn = g.gateExternal;
    const bool keyBound = g.gateKeyBound;   // config, not state
    g = Gate();
    g.gateKeyBound = keyBound;
    // Publish OFF explicitly before going quiet. A reader that only sees the
    // heartbeat stop has to wait out its staleness window; one that is told
    // stops immediately.
    setExternalCameraOnFoot(false);
    if (wasOn) {
        Log::get().note("head offset OFF: the gate was reset, so every latch and "
                        "count is cleared rather than frozen where it stood.");
    }
}

void headOffsetGateSetKeyBound(bool bound) { g.gateKeyBound = bound; }

void headOffsetGateSetNextKeyBound(bool bound) { g.gateHaveNextKey = bound; }

void headOffsetGateSetOnFootLive(bool known, bool onFoot, uint32_t sample) {
    g.liveOnFootKnown = known;
    g.liveOnFoot = onFoot;
    g.liveSample = sample;
    // The first true of a foot session retires the disembark grace: from
    // here the flag describes THIS leg, and false means boarding again.
    if (known && onFoot) g.liveOnFootSeenThisFoot = true;
}

void headOffsetGateSetWakeLive(bool known, bool inSupercruise, bool inTunnel) {
    if (!known) {
        // No status file: remember nothing, so the first real sample after it
        // comes back is not read as an edge it never saw.
        g.wakeKnown = false;
        return;
    }
    const bool wake = inSupercruise || inTunnel;
    const bool rising = g.wakeKnown && wake && !g.wakeLast;
    g.wakeKnown = true;
    g.wakeLast = wake;
    if (!rising || g.gateViewIndex == 0) {
        // Already at 0 is not news, and saying so at every jump would bury
        // the times it actually moved.
        return;
    }
    Log::get().note(
        "head offset: a wake -- the counted view goes from %d back to 0. "
        "Entering supercruise or jumping rebuilds the camera and the game's "
        "preset goes with it (field, 2026-09-02). This is the one boundary "
        "that does; a landing does not.",
        g.gateViewIndex);
    g.gateViewIndex = 0;
    g.gateBridgeStarted = false;
}

uint32_t headOffsetGateEnterCount() { return g.gateCameraEnters; }

int headOffsetGateCountedView() { return g.gateViewIndex; }

void headOffsetGateNewFootSession(const char* source, bool journalSaysSo) {
    // The grace opens OUTSIDE the dedupe: when the panel heuristic spoke
    // first, the journal's later echo is a duplicate reset but not duplicate
    // news -- it still says the status file is lagging this landing. The
    // grace is inert once the flag has been seen true, so a late open costs
    // nothing.
    if (journalSaysSo) {
        g.footGraceJournal = true;
        g.footGraceFrame = g.gateFrameNo ? g.gateFrameNo : 1;
        g.footGraceMs = stampMs();
    }
    // ONE BOUNDARY, POSSIBLY TWO DETECTORS. The journal's Disembark and the
    // panel-return heuristic both mark the same landing, SECONDS apart --
    // whichever speaks first does the work and the other stands down, so the
    // log carries one line per landing rather than an echo.
    //
    // "Seconds apart" is why this is a clock and not the 900 frames it was:
    // the two detectors are separated by how long the game takes to animate an
    // airlock, which does not speed up on a 120Hz headset. At 120Hz the old
    // dedupe closed after 7.5s and the second detector's echo got through as a
    // fresh landing, resetting the view index a second time.
    if (elapsedMs(g.lastFootResetMs, kNewFootSessionMs)) {
        g.lastFootResetMs = 0;   // the window has passed; this is real news
    }
    if (g.lastFootResetMs != 0) return;
    g.lastFootResetMs = stampMs();
    // THE COUNT IS NOT TOUCHED HERE ANY MORE. See the header: the field says
    // a landing leaves the on-foot preset where it was, and the reset that
    // used to live on this line was throwing away a correct number at every
    // airlock.
    g.gateBridgeStarted = false;   // any held view belongs to the old session
    g.liveOnFootSeenThisFoot = false;   // this session's flag not yet observed
    Log::get().note(
        "head offset: a new on-foot session (%s). The counted view stays at "
        "%d -- the game keeps your camera preset across a landing. A wake is "
        "what resets it.",
        source, g.gateViewIndex);
}

void headOffsetGateNoteEmbark() { g.footGraceJournal = false; }

void headOffsetGateKeyPressed() {
    g.gateHaveKey = true;
    // A press while the FLAT PANEL is up can only mean "enter".
    //
    // A blind toggle assumes this module's idea of where the player is has
    // stayed in step with the player's, and any extra or missed press inverts
    // it for the rest of the session. Measured: two presses 1.2 s apart while
    // entering the camera -- the panel had not stopped yet, so the second one
    // was made from first person -- and the toggle read the second as leaving.
    // Every later entry then needed an even number of presses to work.
    //
    // The panel is proof, not inference: it is composited in first person and
    // not in the external camera (6aa.4). While it is up the player is
    // demonstrably NOT in the camera, so there is nothing to toggle out of and
    // a press means one thing. The toggle survives only where the panel cannot
    // answer -- in the camera, where pressing again is genuinely "leave".
    const bool panelUpNow = g.gateSincePanel <= 2 && g.gatePanelRun > 0;
    if (panelUpNow && !g.gateInCamera) {
        g.gateIntent = true;
    } else {
        g.gateIntent = !g.gateIntent;
    }
    // The AGE is reset here, and it was not, which broke the second entry into
    // the camera in every session.
    //
    // gateIntentAge means "frames since the key was pressed", and a press that
    // does not reset it is not a press. The first press of the session worked
    // because the field starts at 0. It then accrued for the whole time the
    // offset was on -- 10170 frames, measured -- and nothing zeroed it, so the
    // NEXT press was born already older than the grace period. The panel branch
    // ran on that same frame, found an intent 10170 frames stale, and cleared
    // it. From the headset: the offset works once per launch, and after you
    // board a ship it never comes back.
    //
    // Both directions, deliberately. A press is a press whichever way it moves
    // the toggle, and a CLEAR that leaves a stale age behind poisons the SET
    // that follows it, which is exactly what happened here.
    g.gateIntentAge = 0;
    g.gateIntentMs = stampMs();
    // The view count is NOT reset here, and that is a correction.
    //
    // It was, on the assumption that the camera opens on its first view every
    // time. The user observed otherwise: once the view is changed the game
    // REMEMBERS it, and the next toggle lands on the same one. So resetting to
    // 0 on entry does not resynchronise the count, it desynchronises it by
    // exactly however far the player had cycled before.
    Log::get().note("external camera key pressed: intent %s%s. View index still %d "
                    "(the game keeps the view across camera toggles and across "
                    "landings; a low or high wake is what resets it).",
                    g.gateIntent ? "SET -- the head offset may arm when the flat "
                                   "panel stops"
                                 : "CLEARED -- the head offset comes off now",
                    panelUpNow && !g.gateInCamera
                        ? " (the flat panel is up, so this can only be an entry)"
                        : "",
                    g.gateViewIndex);
}

// EVERYTHING THAT LANDS IN THE COUNTED VIEW COMES THROUGH HERE.
//
// The on-foot cycle is the only one this gate cares about, and it is 6 long:
// 0..5, rolling over at both ends. Other contexts are longer -- 8 in an SRV,
// up to 11 in a ship and varying with its seat count (measured 2026-09-02) --
// and the game appears to fold a larger index back into this range rather
// than carry it, so folding is what we do too.
//
// Modulo rather than a pair of clamps. Clamping is right only for a step of
// exactly one from inside the range, which is all the count could ever do
// while stepping was its only writer. The read is the other writer, and it
// can hand over an index from a context with a longer ring; that is a value
// to fold, not to clamp to 5 and quietly call the last preset. C++ keeps the
// sign of the dividend, so the negative case needs the second line.
// The ring in force right now. 1..4 are JournalVehicle's OnFoot, Srv, Ship
// and Fighter; 0 is Unknown, which keeps the on-foot ring because that is
// what a rig with no status file has always used.
//
// A fighter borrows the ship's length. It is a guess rather than a
// measurement, and it is the one context nobody has counted -- said here
// rather than left for someone to infer from the absence of a setting.
int currentRing() {
    switch (g.gateVehicle) {
        case 2:  return g.gateViewCountSrv;
        case 3:
        case 4:  return g.gateViewCountShip;
        default: return g.gateViewCount;
    }
}

int normalizeView(int v) {
    const int n = currentRing();
    // 0 is a legitimate configuration meaning "do not wrap", and a negative
    // view is not a view under any configuration.
    if (n <= 0) return v < 0 ? 0 : v;
    v %= n;
    if (v < 0) v += n;
    return v;
}

// Both directions share this, because a ring walked one way and a ring walked
// the other are the same ring, and giving them separate arithmetic is how they
// drift apart.
void headOffsetGateSetVehicle(int journalVehicle) {
    if (journalVehicle == g.gateVehicle) return;
    const int wasRing = currentRing();
    const int was = g.gateViewIndex;
    g.gateVehicle = journalVehicle;
    const int nowRing = currentRing();
    if (wasRing == nowRing) return;
    // Fold across the transition, exactly as the game does: an SRV index of 7
    // is a real preset there and cannot be one on foot, so on stepping out it
    // becomes 1. Only a SHRINKING ring changes anything -- 2 on foot is still
    // 2 in an SRV -- but normalize handles both and says so once either way.
    g.gateViewIndex = normalizeView(g.gateViewIndex);
    if (g.gateViewIndex != was) {
        Log::get().note(
            "external camera view: %d folds to %d -- the cycle is %d long here "
            "and was %d, and the game carries one index across the two.",
            was, g.gateViewIndex, nowRing, wasRing);
    }
}

void headOffsetGateStepView(int delta) {
    g.gateHaveNextKey = true;

    // A press off foot IS counted -- it walks a longer ring, and currentRing()
    // says which. Skipping them was the first correction to the forward-only
    // ratchet and it was itself wrong: the game keeps one index across every
    // context, so a press in an SRV moves the on-foot preset too, just round
    // a ring of 8 rather than 6 before it folds back.
    // Both ends. Going below zero is what the forward-only version never had
    // to think about, and stopping at zero rather than rolling round would
    // put the count one behind for the rest of the session, at the one moment
    // the player is trying to get back to a view they know.
    g.gateViewIndex = normalizeView(g.gateViewIndex + delta);
    // NOT "leave the camera and re-enter". That advice was true only while a
    // landing was believed to reset the index; the game keeps the view across
    // camera toggles, so a toggle resynchronises nothing. A wake genuinely
    // does put the game back to 0, which makes it the honest answer.
    Log::get().note("external camera view -> %d%s (wanted %s). Counted from "
                    "keypresses, not read from the game -- if this disagrees "
                    "with what you see, a jump or a drop to supercruise puts "
                    "both back to 0.",
                    g.gateViewIndex,
                    g.gateViewCount > 0 ? "" : " (not wrapping; set "
                                               "fix.head_offset_view_count)",
                    g.gateWantView < 0 ? "any" : "one specific view");
}

void headOffsetGateViewBumped() { headOffsetGateStepView(+1); }

void headOffsetGateViewUnbumped() { headOffsetGateStepView(-1); }

void headOffsetGateSetView(int view) { g.viewOverride = view; }

bool headOffsetGateWantsPanel() { return g.gateWantsPanel; }

bool headOffsetGateInCamera() { return g.gateInCamera; }

bool headOffsetGatePanelSettled() { return g.panelSettled; }

void headOffsetGateFrame(uint32_t frameNo, uint32_t panelDraws, uint32_t eyeDraws) {
    g.gateFrameNo = frameNo;    // the clock NewFootSession dedupes against
    g.panelSettled = false;     // recomputed every frame, below
    if (!g.gateWantsPanel) {
        // Switched off: CLEAR, do not freeze.
        //
        // This used to be a bare early return, which stopped the exits, the
        // ageing and the heartbeat all at once and left gateInCamera true. The
        // reader gave up a second later, so the offset was still applied for
        // that second -- and switching the gate back on republished the stale
        // latch wherever the player was by then, which for a hot-reload during
        // play means the cockpit.
        if (g.gateExternal || g.gateInCamera) headOffsetGateReset();
        return;
    }

    const bool panelNow = panelDraws > 0;
    const bool sceneNow = eyeDraws > kSceneDraws;

    // Say the panel is being counted, once, and how long it took to start.
    //
    // "The panel composite path was reached" is logged by the vScreen fix
    // and means the DISTANCE OVERRIDE found it. Whether the GATE is counting
    // the same frames is a separate fact, and assuming they were the same
    // cost a session: the override's note appeared and the gate's counter
    // was never confirmed to have moved at all.
    // The panel has been up steadily. NOT ANDed with sceneNow: on foot the
    // world is drawn to the panel, so a full scene in the eyes is what happens
    // when the panel stops, and requiring both asked for a state that barely
    // occurs. Proved by absence -- 90 settled panel frames and no scan.
    g.panelSettled = panelNow && g.gatePanelRun > 30;

    if (panelNow && !g.gatePanelSeenNoted) {
        g.gatePanelSeenNoted = true;
        Log::get().note("head offset gate: the flat panel is being counted "
                        "(first seen at frame %u). It needs 30 such frames "
                        "before it will arm.", frameNo);
    }

    if (panelNow) {
        // A NEW ON-FOOT SESSION: the panel is back after a vehicle leg, and
        // the game keeps its external-camera view across that boundary.
        // "The game remembers the view across toggles" -- printed on every
        // press -- turned out to be true only WITHIN an on-foot session:
        // at every observed second landing the game was back on view 0 while
        // EDVR held or counted the old view, so the offset appeared on
        // preset 0 (6ay, ninth flight, seen directly). Restarting the count
        // at 0 here anchors it to the game's own reset, exactly as launch
        // does -- presses then track every view change with no read needed.
        //
        // This is the HEURISTIC detector for that boundary; the journal's
        // Disembark (wired in device_hook) is the authoritative one and
        // usually speaks first. Both call the same reset, which dedupes.
        if (elapsedMs(g.awaySceneMs, kNewFootSessionMs)) {
            headOffsetGateNewFootSession(
                "the on-foot screen returned after a ship or vehicle leg");
        }
        g.gateAwayScene = 0;
        g.awaySceneMs = 0;
        g.gateSincePanel = 0;
        g.panelStoppedMs = 0;
        g.gateIdleFrames = 0;
        g.idleMs = 0;
        // The gap's peak belongs to the gap. Carrying it across the panel's
        // return would report the last camera stay's number against the next
        // entry, which is the one thing a diagnostic must never do.
        g.gapEyeDraws = 0;
        if (g.gatePanelRun < 10000) ++g.gatePanelRun;
        // The panel overrules the key, but NOT immediately -- and that
        // "not immediately" is the whole bug.
        //
        // The rule is right: the flat panel being composited is direct
        // evidence of first person, so a stale intent must not survive it,
        // or one missed keypress arms the offset for the session.
        //
        // Applied on the very next frame, it destroyed the intent it was
        // meant to guard. Pressing the camera key does not stop the panel;
        // the game takes tens of frames to change mode, measured at 25 to
        // 86. Every one of those frames is a panel frame, so the intent was
        // cleared long before the panel stopped, and the arm test -- which
        // only runs once the panel HAS stopped -- never saw it set. That is
        // why the gate has not armed since intent was introduced, and why
        // it worked before: the old test did not depend on intent at all.
        //
        // So a keypress gets a grace period to take effect. If the panel is
        // still there after it, the press evidently did not enter the
        // camera and the intent is genuinely stale.
        //
        // The two cases are separated because only one of them is waiting
        // for anything. Below, the grace period applies to a press that has
        // NOT armed the gate; a press that has is finished with grace.
        if (g.gateInCamera) {
            // Both, always. The arm/disarm decision below is derived from
            // gateInCamera, so clearing only the published flag would let it
            // be set straight back on this same frame.
            g.gateInCamera = false;
            g.gateExternal = false;
            // And the intent, unconditionally, with no grace period.
            //
            // The panel returning while the offset was on IS the player
            // leaving the camera; there is nothing left to wait for. Leaving
            // the intent SET would make the next press toggle it CLEAR, and
            // the offset would never arm again -- the same failure as a
            // stale age, reached by a different route. Before this it was
            // cleared only as a side effect of the age having grown past the
            // grace period, which is luck, not a rule.
            g.gateIntent = false;
            g.gateIntentAge = 0;
            g.gateIntentMs = 0;
            ++g.gateExits;
            edvr::setExternalCameraOnFoot(false);
            Log::get().note("head offset OFF: the flat panel is back, so this is "
                            "on-foot first person again (%u frame(s) in the "
                            "external camera).", g.gateSinceEnter);
        } else if (g.gateIntent && elapsedMs(g.gateIntentMs, g.gateIntentGraceMs)) {
            const uint32_t age = g.gateIntentAge;  // reported, then reset
            g.gateIntent = false;
            g.gateIntentAge = 0;
            g.gateIntentMs = 0;
            Log::get().note("external camera intent cleared: the flat panel is "
                            "still up %u frames after the key, so that press did "
                            "not enter the camera.", age);
        }
    } else if (g.gateInCamera && g.gateHaveKey && !g.gateIntent) {
        // The player pressed their camera key while the offset was on, so
        // they have left the camera -- whatever they left it FOR. This is
        // the exit the render state cannot supply: boarding a ship produces
        // no panel frame ever, so before this the latch simply stayed on
        // and the offset followed the player into the cockpit.
        g.gateInCamera = false;
        g.gateExternal = false;
        ++g.gateExits;
        edvr::setExternalCameraOnFoot(false);
        Log::get().note("head offset OFF: the external camera key was pressed "
                        "again (%u frame(s) in). Not waiting for the flat panel, "
                        "which never comes back if you left for a ship.",
                        g.gateSinceEnter);
    } else {
        ++g.gateSincePanel;
        if (eyeDraws > g.gapEyeDraws) g.gapEyeDraws = eyeDraws;
        // The moment the panel stopped, in Status samples: keyless arming
        // requires a FRESHER on-foot sample than this, so a stale second of
        // "on foot" cannot arm the offset into a boarding animation.
        // Stamped on the same edge, for the tests that ask how long ago.
        if (g.gateSincePanel == 1) {
            g.sampleAtPanelStop = g.liveSample;
            g.panelStoppedMs = stampMs();
        }
        // The window is TIGHT, and measured rather than chosen.
        //
        // Both real entries logged "the flat panel stopped 2 frame(s) ago".
        // Entering the external camera is effectively instantaneous: the
        // panel composite stops and the scene appears within a couple of
        // frames.
        //
        // It was 90, which is not a window so much as an absence of one,
        // and it let in every other way of leaving the panel. Boarding the
        // ship is the measured case -- panel stops, cockpit draws a full
        // scene, gate opens, and since the panel never comes back in a ship
        // it stayed open. Leaving HMD Cinema Mode has the same signature
        // for the same reason: 6aa.2 established Cinema Mode runs the panel
        // composite too, so exiting it is also "panel stops, full scene".
        //
        // A tight window does not identify the external camera -- nothing
        // here does -- but it does require the transition to be as abrupt
        // as the real one, which the slower mode changes are not.
        // With a key bound, the player's press is also required. That turns
        // "the panel stopped and a scene appeared" from the whole test into
        // a corroboration of something they actually did, which is what
        // separates entering the camera from boarding a ship or leaving
        // Cinema Mode -- those produce the same render state and no press.
        // The window applies ONLY when there is no key to go on.
        //
        // Measured across three sessions, the gap between the flat panel
        // stopping and a full scene appearing was 2 frames, then 6, then
        // 61. It is not a constant and it is not something to tune: it
        // depends on where the player is and what the transition has to
        // load. Every value picked for it so far has been picked from the
        // last session that worked and been wrong for the next one.
        //
        // With hotkey.external_camera bound, the player has already said
        // what they did. The window was only ever standing in for that --
        // rejecting boarding and Cinema-Mode-exit, which look the same in
        // render state -- so once intent exists the window is not a weaker
        // discriminator, it is a redundant one that can only cause misses.
        //
        // Without a key it is still the only thing in the way, so it stays
        // for that case.
        // ARMING NEEDS POSITIVE EVIDENCE, and this is the change that makes
        // the whole feature safe rather than merely usually right.
        //
        // It used to arm on absence: the panel stopped, a full scene is being
        // drawn, and that happened within a window. But 6ac.6b already REFUTED
        // that as a discriminator -- entering the external camera, boarding a
        // ship and leaving HMD Cinema Mode are indistinguishable in render
        // state -- and 6ac.6c refuted the window as the tiebreak, measuring it
        // at 2, 6, 61 and 2 frames across four sessions.
        //
        // So an unconfigured install would arm on the player walking into their
        // own ship, and move their viewpoint into the cockpit until a
        // 30-second ceiling took it off. The consequence class here is "the
        // viewpoint moved in the wrong mode"; evidence that cannot distinguish
        // the modes does not support it, whatever the timing.
        //
        // The player's keypress is the missing information, not a missing
        // heuristic. Without a bound key the gate now does NOTHING, which is
        // what the ini has always claimed and what makes a fresh install
        // genuinely inert. The window survives only as a sanity bound on top.
        const bool intentOk = g.gateKeyBound && g.gateIntent;
        // KEYLESS INTENT, from the game's own word instead of a binding. The
        // key exists because render state alone cannot tell entering the
        // camera from boarding a ship (6ac.6b) -- but the game can: Status's
        // OnFoot flag HOLDS through the whole camera window (6bb, measured
        // across eight view changes) and drops on boarding, which also
        // announces Embark. So: still on foot per the game, with the on-foot
        // screen gone and a stereo scene up, is the external camera. Only
        // when no camera key is bound -- a bound key keeps exactly its old
        // meaning -- and only while the live context is KNOWN; menus, a
        // missing Status.json or the watcher being off all answer unknown,
        // which restores the bind-a-key requirement precisely.
        // Two ways to know the player is on foot: a fresh Status.json sample,
        // or the journal's Disembark inside its grace window -- the file lags
        // that boundary by ~6 measured seconds, and entries made during the
        // lag were being lost (11:02:28 armed only because the player took
        // 6.7 s to enter; the 60-frame windows before that were forfeit).
        // The grace dies at the first true sample, at Embark, or at expiry;
        // physics closes the boarding hole -- a re-board requires standing
        // first, and standing is the true sample that ends the grace.
        const bool statusFresh =
            g.liveOnFootKnown && g.liveOnFoot &&
            g.liveSample > g.sampleAtPanelStop;
        const bool graceActive =
            g.footGraceJournal && !g.liveOnFootSeenThisFoot &&
            !elapsedMs(g.footGraceMs, kFootGraceMs);
        const bool autoIntent = g.gateKeylessOn && !g.gateKeyBound &&
                                (statusFresh || graceActive);
        if (autoIntent && !g.autoIntentNoted) {
            g.autoIntentNoted = true;
            Log::get().note(
                "head offset: no external-camera key is bound, and the game's "
                "own status says you are on foot -- so entering the external "
                "camera will be detected from that instead. A keyboard "
                "binding in Elite still gives the crispest entries.");
        }
        const bool timingOk = !elapsedMs(g.panelStoppedMs, g.gateEnterWindowMs) ||
                              !elapsedMs(g.gateIntentMs, g.gateIntentGraceMs);
        // The panel must have been gone for a WHILE, not for one frame.
        //
        // sincePanel >= 1 satisfied the window, so a single dropped panel frame
        // -- a hitch, a stutter, one composite missed -- armed the gate for
        // exactly as long as it took the panel to come back. That is a
        // one-frame pose jump of whatever the offset is, and the panel's return
        // then both exited AND ate the pending intent, so the entry the player
        // actually asked for was silently discarded too. One hitch inside the
        // grace period cost a jolt and a missed entry.
        //
        // The mode change itself takes 25 to 86 frames (6ac.6c), so requiring
        // several panel-less frames costs nothing real and rejects every
        // single-frame gap.
        const bool panelGoneAWhile = g.gateSincePanel >= 8;
        const bool entryAsked =
            (intentOk && timingOk) ||
            (autoIntent && !elapsedMs(g.panelStoppedMs, kKeylessEnterMs));
        if (!g.gateInCamera && entryAsked && panelGoneAWhile &&
            sceneNow && g.gatePanelRun > 30) {
            g.gateInCamera = true;
            ++g.gateCameraEnters;
            g.gateSinceEnter = 0;
            g.heartbeatMs = stampMs();
            Log::get().note(
                "on-foot external camera: the flat panel stopped %u frame(s) ago "
                "after %u settled frames, and %u draws are reaching the eye "
                "textures. View index %d%s.",
                g.gateSincePanel, g.gatePanelRun, eyeDraws,
                g.gateViewIndex,
                intentOk ? "" : " (entered keylessly, from the game's own "
                                "on-foot status)");
        }
        // BOARDING EXITS THE CAMERA, by the game's own word. The player can
        // board their ship directly from the external camera; the panel never
        // returns and no key is pressed, which is exactly the latch-stuck
        // case the camera key existed to close. The live OnFoot flag dropping
        // closes it without one -- and it must beat the offset, because an
        // offset that follows the player into the cockpit is the failure this
        // module exists to prevent.
        // Only a flag that has been TRUE this foot session gets to say the
        // player left it. Right after a disembark the file still answers for
        // the previous leg (~6 s measured), and firing on that stale false
        // killed the latch six frames running while the player stood in the
        // camera (10:57:12). False-before-first-true is history, not news.
        if (g.gateInCamera && g.liveOnFootKnown && !g.liveOnFoot &&
            g.liveOnFootSeenThisFoot) {
            g.gateInCamera = false;
            g.gateExternal = false;
            ++g.gateExits;
            edvr::setExternalCameraOnFoot(false);
            Log::get().note(
                "head offset OFF: the game's status says you are no longer on "
                "foot, so the external camera is over -- boarded, most likely. "
                "Coming off before the cockpit does.");
        }
        // Neither panel nor scene for a long stretch: a menu, a loading
        // screen, or a mode change we cannot see. Drop the latch rather
        // than carry it into whatever comes back, because the one thing
        // worse than the offset not applying is it applying in the cockpit.
        if (!sceneNow) {
            ++g.gateIdleFrames;
            if (g.gateIdleFrames == 1) g.idleMs = stampMs();
            // Timed, not counted. A loading screen is the headline case for
            // this branch and it is precisely where frames stop tracking
            // time: 300 of them went by in 170ms at the measured 1790fps, so
            // the latch was dropped almost the instant a load began rather
            // than after the deliberate pause this was written to wait out.
            if (elapsedMs(g.idleMs, kIdleDropMs) && g.gateInCamera) {
                g.gateInCamera = false;
                g.gateExternal = false;
                ++g.gateExits;
                edvr::setExternalCameraOnFoot(false);
                Log::get().note("head offset OFF: neither the panel nor a drawn "
                                "scene for %u frames over %llu ms, so the latch "
                                "is being dropped rather than guessed.",
                                g.gateIdleFrames,
                                static_cast<unsigned long long>(nowMs() - g.idleMs));
            }
        } else {
            g.gateIdleFrames = 0;
            g.idleMs = 0;
            // Vehicle time: a full scene, no panel, and not the camera. This
            // is what accrues toward the new-session boundary at the panelNow
            // block -- a camera stint is excluded by gateInCamera, and a map
            // or menu is idle rather than scene, so only ship and SRV legs
            // count.
            if (!g.gateInCamera && g.gateAwayScene < 1000000) {
                ++g.gateAwayScene;
                if (g.gateAwayScene == 1) g.awaySceneMs = stampMs();
            }
        }

            // THE CEILING IS GONE, and its knob with it.
            //
            // It bounded a latch that could not tell it had left the camera,
            // which was real while the gate armed on render state alone.
            // Arming now requires a bound key, and that key is also the exit,
            // so the case it guarded cannot arise -- its own condition,
            // !gateHaveKey && gateInCamera, had become unreachable.
            //
            // Left in place it did active harm rather than nothing: 3600
            // frames is thirty seconds, the verified hold in this camera is
            // 9833, and once it fired there was no way to re-arm without
            // leaving and re-entering. A backstop that can only fire on
            // correct behaviour is not a backstop.

        // While latched, say what the frame looks like, occasionally.
        //
        // The transition logging alone cannot answer why the latch is stuck:
        // it says when the state changed and nothing about the frames in
        // between. This is what a future discriminator would be built from.
        // Stamped at entry rather than zeroed, so the first heartbeat lands one
        // interval IN rather than on the entry frame itself -- the old
        // `gateSinceEnter % 600` skipped 0 for the same reason, and a zeroed
        // stamp would print a "still on" line immediately under "ON".
        if (g.gateExternal && g.gateSinceEnter > 0 &&
            elapsedMs(g.heartbeatMs, kHeartbeatMs)) {
            g.heartbeatMs = stampMs();
            Log::get().note("head offset still on: %u frames in, %u draws into "
                            "the eyes, %u frames since the panel.",
                            g.gateSinceEnter, eyeDraws,
                            g.gateSincePanel);
        }
        // Too long since the panel for this to be a transition FROM it.
        // On-foot credit dies with the panel, not 300 frames later.
        //
        // 300 frames of grace let the ship-vanity route survive: board the ship,
        // press the camera key within ~3 seconds, and the gate still believed
        // the player was settled on foot -- so the on-foot offset armed on the
        // SHIP camera. The credit existed to ride out brief panel gaps, and
        // panelGoneAWhile above is now what does that job, for eight frames
        // rather than three hundred.
        //
        // A second is still generous for a stutter and far short of a mode
        // change anybody could act on. It was 90 frames, and "generous for a
        // stutter" is a claim about milliseconds: 750 of them at 120Hz.
        if (elapsedMs(g.panelStoppedMs, kPanelRunOverMs)) {
            // Say when an entry was REJECTED, and by which number.
            //
            // Without this a gate that never opens is indistinguishable from
            // a gate that is never asked, which is the whole difficulty of
            // this feature: entering the external camera, boarding the ship
            // and leaving Cinema Mode all look like "the panel stopped and a
            // scene appeared", and only the numbers separate them. These
            // lines are what a real discriminator would be built from.
            //
            // This used to fire only when no key was bound, which suppressed
            // it in exactly the configuration being debugged -- so that
            // flight produced a gate that did not arm and NOTHING saying
            // which test failed. Printing all the values costs one line and
            // removes the guessing: whichever one reads short is the bug.
            //
            // IT ALSO USED TO SIT BEHIND sceneNow, WHICH IS THE ONE
            // CONDITION MOST LIKELY TO BE THE CULPRIT. Two field sessions
            // (2026-08-19, a Steam install chaining EDHM and a dxgi.dll
            // wrapper) had eye-draw counts that never passed 20 all session
            // -- menu magnitude, against the 975 and 1074 session peaks
            // measured on the two headsets here -- so the gate could not arm
            // however the player pressed, and the line written to say so was
            // gated on the very test that was failing. Both logs went out
            // with no line naming eyeDraws at all, and the answer had to be
            // inferred from the vScreen totals three modules away. A
            // diagnostic must not require the thing it diagnoses.
            //
            // ONCE PER GAP, AT THE MOMENT THE CHANCE IS LOST, rather than
            // once per frame. Dropping sceneNow without this would have
            // spent the whole 20-line budget in a fifth of a second the
            // first time the panel stopped -- 20 consecutive frames at
            // 90Hz -- which is the same silence by a different route. Here
            // the on-foot credit has just expired: the arm test wants
            // panelRun > 30 and this line runs on the frame before it is
            // zeroed, so it fires exactly when the entry window closes
            // unarmed, and at most once for each. gateHaveKey keeps it out
            // of startup, where a budget was spent at frame 3062 last time
            // with panelRun=0 and no key yet pressed.
            //
            // WHAT THIS GIVES UP: a gap shorter than the credit never gets
            // here at all, so sincePanel is always past its own floor by the
            // time the line prints and can no longer be the number that reads
            // short. That case is the one-frame hitch, which cannot be an
            // entry anyway -- the mode change alone is 25 to 86 frames
            // (6ac.6c) -- and a press left hanging by one already has its own
            // line from the panel branch above.
            if (g.gatePanelRun > 30 && !g.gateInCamera && g.gateHaveKey &&
                g.gateNearMisses < 20) {
                ++g.gateNearMisses;
                Log::get().note(
                    "head offset NOT armed and the entry window has now "
                    "closed: panelRun=%u (needs >30), sincePanel=%u (needs "
                    ">=8, window %u ms), eyeDraws peaked at %u in this gap "
                    "(needs >%u), intent=%s, key %s, pressed %s, view=%d "
                    "(wants %d). Whichever of those reads short is why the "
                    "camera did not take.%s",
                    g.gatePanelRun, g.gateSincePanel,
                    (uint32_t)g.gateEnterWindowMs, g.gapEyeDraws, kSceneDraws,
                    g.gateIntent ? "set" : "CLEAR",
                    g.gateKeyBound
                        ? "BOUND"
                        : (g.gateKeylessOn && g.liveOnFootKnown
                               ? "not bound (keyless: the game's on-foot "
                                 "status stands in)"
                               : "NOT BOUND -- no keyboard camera key found "
                                 "in your Elite bindings; bind one in Elite "
                                 "and it is picked up within seconds"),
                    g.gateHaveKey ? "yes" : "not yet this session",
                    g.gateViewIndex, g.gateWantView,
                    // Only where the count IS the short one. Printed always,
                    // it explained an intent that had expired as a graphics
                    // problem, which is a log line telling the reader the
                    // wrong module to go and look at.
                    g.gapEyeDraws <= kSceneDraws
                        ? " That peak is the short one here, and it is not "
                          "something you did: it means the world is not being "
                          "drawn into anything the size of an eye texture. The "
                          "vScreen totals in this log carry the same number for "
                          "the whole session."
                        : "");
            }
            g.gatePanelRun = 0;
        }
    }
    // The offset is armed from two SEPARATE facts, and keeping them apart
    // is what lets the view change matter.
    //
    //   gateInCamera  the player is on foot in the external camera. Entered
    //                 on the transition, held until the panel returns or the
    //                 camera key says otherwise.
    //   the view      which of the camera's views is showing, counted from
    //                 keypresses.
    //
    // Merging them into one latch meant the view could only be judged during
    // the 5-frame entry window. Cycling to the intended view AFTER entering
    // -- which is exactly how it is used, since the camera opens on the
    // portrait view and the useful one is the next -- could then never arm
    // it, because by then the window was long closed.
    // The GAME's view if somebody can read it, the keypress count otherwise.
    //
    // The count is ANCHORED, which 6ac.6d originally denied and has been
    // corrected: the game's view resets to 0 at every launch and this
    // counter starts at 0 when the proxy loads, so they are in step by
    // construction. What the count cannot survive is a MISSED press, which
    // desyncs it silently for the rest of the session.
    //
    // That is what an override is for, and it is strictly better where one
    // exists: it needs no key bound at all, it cannot drift, and it
    // corrects a count that already has.
    //
    // -1 means "do not know" -- not scanned yet, no records found, or a
    // value outside the plausible range -- and falls back to the count
    // rather than substituting a number that happens to be wrong.
    const int gameView = g.viewOverride;
    // LOSING the view is a reason to stop -- after a bridge, not instantly.
    //
    // The strict rule was: once something HAS been supplying the view, losing
    // it drops the offset until it comes back, because a count with no origin
    // cannot be trusted (6ac.6d) and riding on a stale index moves the
    // viewpoint on a guess. That rule met the field in 6ar-6at: near a planet
    // the game rebuilds its camera records every ten to thirty seconds, the
    // read dies at every rebuild, and the drop lands exactly when the player
    // is sitting still in the wanted view USING the offset -- supplying none
    // of the presses re-certification needs. The offset blinked off mid-use,
    // every half minute, through no input at all.
    //
    // THE BRIDGE: for a bounded window after the read dies, the counted view
    // stands. It is not the originless count the strict rule refused -- it
    // was synced to a confirmed read seconds ago, presses still advance it
    // (where the next-view key is bound), the first successful re-read
    // corrects it through the sync below, and expiry restores the strict
    // behaviour. The exposure is a view change nobody could see during the
    // window, held at most kBridgeFrames or until the next read, whichever
    // is sooner -- chosen (by Sean, 2026-08-15) over the offset dropping at
    // every rebuild.
    if (gameView >= 0) {
        g.gateViewEverRead = true;
        g.gateViewLostNoted = false;
        g.gateBridgeStarted = false;   // any successful read ends the episode
    } else if (g.gateViewEverRead) {
        if (g.gateBridgeOn && !g.gateBridgeStarted) {
            g.gateBridgeStarted = true;
            // NO EXPIRY, and that is a product decision, not an oversight: the
            // player stays in Explorer Cam as long as they wish (2026-08-15),
            // and the held value cannot go stale outside the camera because
            // the game freezes the view there. The exposure that remains is a
            // press nobody saw while IN the camera on a dead read -- the
            // offset then follows the old view until any successful read, and
            // cycling forward re-certifies in three witnessed presses. The
            // wall-clock TTL tried first expired while the player was away
            // and dead bridges greeted every relanding; the in-camera budget
            // tried second contradicted indefinite camera stays.
            Log::get().note(
                "camera view: the read died mid-camera (the game rebuilds its "
                "records near a planet), so the last confirmed view %d is being "
                "held until it comes back. %s",
                g.gateViewIndex,
                g.gateHaveNextKey
                    ? "Your view-key presses still count during the hold."
                    : "No next-view key is bound, so cycling during the hold "
                      "cannot be seen -- if you switch presets before the read "
                      "returns, the offset follows the old one until it does.");
        }
        if (!g.gateBridgeOn && !g.gateViewLostNoted) {
            g.gateViewLostNoted = true;
            Log::get().note("head offset OFF: the camera view can no longer be "
                            "read, so which preset you are on is unknown. It is "
                            "coming off rather than staying on a preset it "
                            "cannot confirm; it will come back when the view "
                            "does.");
        }
    }
    // THE SYNC TRUSTS ONLY WHAT COULD BE TRUE. A read that differs from the
    // held view while the player is OUT of the camera is impossible for the
    // real preset -- the game freezes the view there -- so it is evidence
    // about the SUPPLIER, not about the view (6aw: a counter in the array
    // certified and supplied 3-then-0 while the player stood outside; the
    // sync took both and the bridge faithfully held the poison). In-camera
    // reads sync as always: the player can genuinely cycle there.
    if (gameView >= 0 && gameView != g.gateViewIndex) {
        // BEFORE the in-camera split, so a refused read is recorded too: the
        // question is what the game did, not whether we were willing to
        // believe it. Deduped on the pair rather than rate-limited, so a
        // steady disagreement says its piece once and a changing one is
        // followed exactly.
        if (g.desyncLog &&
            (gameView != g.lastDesyncGame || g.gateViewIndex != g.lastDesyncCount)) {
            g.lastDesyncGame = gameView;
            g.lastDesyncCount = g.gateViewIndex;
            Log::get().note(
                "camera view DESYNC: the game reads %d, the count says %d (%+d). "
                "In camera: %s. On foot: %s. Panel: %u frame(s) since last up, "
                "run %u. Disembark grace: %s. A fall to 0 that you did not press "
                "for is the game resetting its own index, and whatever you were "
                "doing at this timestamp is the boundary worth anchoring to.",
                gameView, g.gateViewIndex, gameView - g.gateViewIndex,
                g.gateInCamera ? "yes" : "no",
                g.liveOnFootKnown ? (g.liveOnFoot ? "yes" : "no") : "unknown",
                g.gateSincePanel, g.gatePanelRun,
                g.footGraceJournal ? "open" : "closed");
        }
        if (g.gateInCamera || !g.gateViewEverRead) {
            if (!g.gateViewSynced) {
                g.gateViewSynced = true;
                Log::get().note("camera view: the game says %d, the keypress count "
                                "said %d. Using the game's from here, so a missed "
                                "press no longer desyncs anything.",
                                gameView, g.gateViewIndex);
            }
            // Folded, not taken raw: a read taken while the game still
            // holds a longer context's index would otherwise put the count
            // somewhere the on-foot cycle cannot reach, and nothing
            // downstream range-checks it.
            g.gateViewIndex = normalizeView(gameView);
        } else {
            if (!g.gateSyncRefusedNoted) {
                g.gateSyncRefusedNoted = true;
                Log::get().note(
                    "camera view: a read said %d while you were not in the camera, "
                    "where the view cannot change -- keeping the confirmed %d and "
                    "treating the reader as suspect. It will be believed again the "
                    "next time it agrees, or the next time you are in the camera.",
                    gameView, g.gateViewIndex);
            }
        }
    } else if (gameView >= 0) {
        g.gateSyncRefusedNoted = false;   // agreement: the reader is sane again
    }
    const bool bridging =
        gameView < 0 && g.gateViewEverRead && g.gateBridgeOn;
    const bool viewLost = g.gateViewEverRead && gameView < 0 && !bridging;
    const bool viewOk = !viewLost &&
        (g.gateWantView < 0 || g.gateViewIndex == g.gateWantView);

    // Say so when the view can NEVER match, and only then.
    //
    // The conditions are checked here rather than at config time because
    // three of them are only knowable in a frame: whether the player has
    // actually reached the camera, whether an override is supplying an
    // index, and whether the count has moved. A build that reads the view
    // from the game needs no key bound at all, and telling its user to bind
    // one -- with "the offset will never arm", while it arms -- is worse
    // than saying nothing.
    //
    // Every clause is load-bearing: in the camera (so it is the moment the
    // player expects something), the view does not match, no key can ever
    // change it, and nobody is supplying it. That combination is a dead
    // configuration and nothing else is.
    if (!g.gateViewWarned && g.gateInCamera && !viewOk && g.gateWantView > 0 &&
        !g.gateHaveNextKey && g.viewOverride < 0) {
        g.gateViewWarned = true;
        // Names the CAUSE rather than a setting to change, because the setting
        // that would change it is not the same in every build. What is true
        // everywhere is that nothing could say which camera view this is.
        Log::get().note(
            "head offset: the camera view could not be read from the game, so "
            "this cannot tell which preset you are on. It wants view %d and is "
            "assuming %d, so the offset will not engage.\n"
            "  The search for the view index found nothing usable -- see the "
            "camera view lines above for how much memory it covered and how "
            "many attempts it made. After a game update, "
            "d3d11.camera_index_type_offset no longer points at the right "
            "thing and needs re-measuring.\n"
            "  advanced.head_offset_view = -1 applies the offset in every camera "
            "preset, including the one that faces back at you.",
            g.gateWantView, g.gateViewIndex);
    }
    const bool wantOffset = g.gateInCamera && viewOk;
    // Published EVERY frame, not only on change, because this call is also
    // the heartbeat openvr_api.dll uses to decide the answer is still being
    // made. This whole block runs inside a fault-budgeted guard on the
    // Present path that stops running permanently after a few faults; the
    // gate would then freeze at its last answer, and a frozen "yes" keeps
    // the offset applied in the cockpit for the rest of the session with
    // nothing logged, because no decision is being re-made to log.
    //
    // Deliberately inside the guard, alongside the decision it vouches for.
    // A heartbeat that outlived the thing it reports on would assert
    // liveness for a gate that had stopped -- worse than no heartbeat.
    edvr::setExternalCameraOnFoot(wantOffset);
    if (wantOffset != g.gateExternal) {
        g.gateExternal = wantOffset;
        if (wantOffset) {
            ++g.gateEnters;
            g.gateSinceEnter = 0;
            g.heartbeatMs = stampMs();
            Log::get().note("head offset ON: on foot in the external camera, "
                            "view %d.", g.gateViewIndex);
            // The offset is applied in openvr_api.dll. If that half is not
            // installed, this side arms, logs the line above, and nothing
            // whatsoever happens to the viewpoint -- and the only message that
            // would explain it lives in the DLL the player skipped.
            //
            // The transition flash fix already warns about its missing half for
            // exactly this reason. This is the same warning and the more
            // necessary one: the flash fix degrades to a detector that still
            // logs, this degrades to nothing at all.
            if (!glitchConsumerPresent() && !g.gateNoConsumerNoted) {
                g.gateNoConsumerNoted = true;
                Log::get().note(
                    "head offset: ...but openvr_api.dll is NOT INSTALLED (or is "
                    "a different EDVR version), and that is the half which "
                    "actually moves the viewpoint. Nothing will happen. Explorer "
                    "Cam needs BOTH files -- see the openvr folder in the release "
                    "and its READ-ME-FIRST.txt.");
            }
        } else {
            ++g.gateExits;
            Log::get().note("head offset OFF: camera view is %d and the offset "
                            "is for view %d (advanced.head_offset_view). Still in "
                            "the camera.", g.gateViewIndex, g.gateWantView);
        }
    }
    // Counts frames IN THE CAMERA, not frames with the offset applied, and
    // the difference is the ceiling's whole reason for existing.
    //
    // It counted gateExternal, so sitting in the camera on a view the
    // offset is not for -- gateInCamera true, gateExternal false -- froze it
    // at zero. The ceiling at the top of this block tests it against
    // gateInCamera, so in exactly that state the backstop could never fire.
    // Board a ship from there with no key bound and the latch is held for
    // the session: the panel never returns, a full scene is drawn every
    // frame so the idle exit never counts, and cycling to the wanted view
    // later turns the offset on IN THE COCKPIT -- the outcome this file
    // already calls the one thing worse than it not applying.
    if (g.gateInCamera) ++g.gateSinceEnter;

    // Age the intent, and expire it WHEREVER the player is.
    //
    // Two bugs in one place. The counter was never incremented at all, so
    // the grace period could not fire and intent, once set, was cleared
    // only by another keypress. And the expiry lived inside the panel
    // branch, so it needed a flat panel on screen to run.
    //
    // Measured cost: a press at frame 2499 -- in a menu, 8000 frames before
    // the panel was first counted -- latched intent, so the real press on
    // foot TOGGLED IT OFF and the head offset never armed for the rest of
    // the session. From the headset that is "the offset just stopped
    // working".
    //
    // A keypress is a request to enter the camera. If the gate has not
    // armed within the grace period, that press did something else, and
    // keeping it only inverts the next real one.
    //
    // It ages only while the gate has NOT armed, which is the only period
    // the number describes. Counting on through a successful camera session
    // is what produced the 10170-frame age that poisoned the next press: an
    // age accrued while the press was WORKING, then tested as though the
    // press had failed.
    if (g.gateIntent && !g.gateInCamera) {
        ++g.gateIntentAge;
        if (elapsedMs(g.gateIntentMs, g.gateIntentGraceMs)) {
            g.gateIntent = false;
            // WHICH KIND of failure, on the line that reports it.
            //
            // The rejection line that names the four numbers is itself behind
            // `sceneNow && panelRun > 30`, so in the one failure where the gate
            // is fed nothing at all -- no eye draws, no panel composites, the
            // vScreen recogniser matching nothing -- it cannot print, and this
            // line was the only trace left. It read as "your press did not
            // count", which sent a player looking at their bindings while the
            // cause was three modules away. A gate that has never seen a single
            // panel draw is not judging presses; it is starved, and the vScreen
            // totals in this same log say so.
            const bool everSawPanel = g.gatePanelSeenNoted;
            Log::get().note("external camera intent expired: %u frames since the "
                            "key with the gate never arming, so that press was "
                            "not an entry into the camera. Cleared, so the next "
                            "press is a fresh one rather than a toggle back.%s",
                            g.gateIntentAge,
                            everSawPanel
                                ? ""
                                : " The gate has not seen the flat panel drawn ONCE this "
                                  "session, so it is not judging your presses -- it has "
                                  "no input. Read the vScreen totals line above: an "
                                  "eye-draw count stuck at 0 means the recogniser is "
                                  "matching nothing, and this is downstream of that.");
            g.gateIntentAge = 0;
            g.gateIntentMs = 0;
        }
    }
}

}  // namespace edvr
