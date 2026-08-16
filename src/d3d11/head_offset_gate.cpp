// GENERATED from src/d3d11/head_offset_gate.cpp in the private edvr repo -- do not edit here.
// Edit there, then: python tools/sync_common.py --write   [body-sha256 db30dfca7d593f33]
#include "head_offset_gate.h"

#include "../common/config.h"
#include "../common/frame_flag.h"
#include "../common/log.h"

namespace edvr {
namespace {

// One instance, file-local. The gate is per-process by nature -- there is one
// player in one mode -- and keeping it out of the render hooks' State is what
// lets the same logic serve both builds instead of being forked into each.
struct Gate {
    bool     gateWantsPanel = false;     // config: is the gate switched on
    uint32_t gatePanelRun = 0;           // consecutive frames settled on the panel
    uint32_t gateSincePanel = 0;         // frames since the panel was last seen
    uint32_t gateIdleFrames = 0;         // frames with neither panel nor scene
    bool     gateExternal = false;       // the latch itself
    uint32_t gateSinceEnter = 0;         // frames since the latch was set
    uint32_t gateEnterWindow = 60;       // frames after the panel stops
    uint32_t gateNearMisses = 0;         // rejected candidates, logged
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
    bool     gateNoConsumerNoted = false;
    uint32_t gateIntentAge = 0;          // frames since the key was pressed
    uint32_t gateIntentGrace = 180;      // frames a press gets to take effect
    bool     gateInCamera = false;       // in the camera, whatever the view
    uint32_t gateEnters = 0, gateExits = 0;

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
    g.gateEnterWindow = static_cast<uint32_t>(
        cfg.getIntInRange("fix.head_offset_enter_window", 60, 0, 100000));
    g.gateIntentGrace = static_cast<uint32_t>(
        cfg.getIntInRange("fix.head_offset_intent_grace", 180, 0, 100000));
    g.gateWantView = cfg.getIntInRange("fix.head_offset_view", 2, -1, 63);
    g.gateViewCount = cfg.getIntInRange("fix.head_offset_view_count", 6, 0, 64);
    g.gateBridgeOn = cfg.getBool("fix.head_offset_view_bridge", true);
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
    // The view count is NOT reset here, and that is a correction.
    //
    // It was, on the assumption that the camera opens on its first view every
    // time. The user observed otherwise: once the view is changed the game
    // REMEMBERS it, and the next toggle lands on the same one. So resetting to
    // 0 on entry does not resynchronise the count, it desynchronises it by
    // exactly however far the player had cycled before.
    Log::get().note("external camera key pressed: intent %s%s. View index still %d "
                    "(not reset -- the game remembers the view across toggles).",
                    g.gateIntent ? "SET -- the head offset may arm when the flat "
                                   "panel stops"
                                 : "CLEARED -- the head offset comes off now",
                    panelUpNow && !g.gateInCamera
                        ? " (the flat panel is up, so this can only be an entry)"
                        : "",
                    g.gateViewIndex);
}

void headOffsetGateViewBumped() {
    g.gateHaveNextKey = true;
    ++g.gateViewIndex;
    if (g.gateViewCount > 0 && g.gateViewIndex >= g.gateViewCount) {
        g.gateViewIndex = 0;
    }
    Log::get().note("external camera view -> %d%s (wanted %s). Counted from "
                    "keypresses, not read from the game -- if this disagrees with "
                    "what you see, leave the camera and re-enter to resynchronise.",
                    g.gateViewIndex,
                    g.gateViewCount > 0 ? "" : " (not wrapping; set "
                                               "fix.head_offset_view_count)",
                    g.gateWantView < 0 ? "any" : "one specific view");
}

void headOffsetGateSetView(int view) { g.viewOverride = view; }

bool headOffsetGateWantsPanel() { return g.gateWantsPanel; }

bool headOffsetGateInCamera() { return g.gateInCamera; }

bool headOffsetGatePanelSettled() { return g.panelSettled; }

void headOffsetGateFrame(uint32_t frameNo, uint32_t panelDraws, uint32_t eyeDraws) {
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
    const bool sceneNow = eyeDraws > 50;

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
        g.gateSincePanel = 0;
        g.gateIdleFrames = 0;
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
            ++g.gateExits;
            edvr::setExternalCameraOnFoot(false);
            Log::get().note("head offset OFF: the flat panel is back, so this is "
                            "on-foot first person again (%u frame(s) in the "
                            "external camera).", g.gateSinceEnter);
        } else if (g.gateIntent && g.gateIntentAge > g.gateIntentGrace) {
            const uint32_t age = g.gateIntentAge;  // reported, then reset
            g.gateIntent = false;
            g.gateIntentAge = 0;
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
        const bool timingOk = g.gateSincePanel <= g.gateEnterWindow ||
                              g.gateIntentAge <= g.gateIntentGrace;
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
        if (!g.gateInCamera && intentOk && timingOk && panelGoneAWhile &&
            sceneNow && g.gatePanelRun > 30) {
            g.gateInCamera = true;
            g.gateSinceEnter = 0;
            Log::get().note(
                "on-foot external camera: the flat panel stopped %u frame(s) ago "
                "after %u settled frames, and %u draws are reaching the eye "
                "textures. View index %d.",
                g.gateSincePanel, g.gatePanelRun, eyeDraws,
                g.gateViewIndex);
        }
        // Neither panel nor scene for a long stretch: a menu, a loading
        // screen, or a mode change we cannot see. Drop the latch rather
        // than carry it into whatever comes back, because the one thing
        // worse than the offset not applying is it applying in the cockpit.
        if (!sceneNow) {
            if (++g.gateIdleFrames > 300 && g.gateInCamera) {
                g.gateInCamera = false;
                g.gateExternal = false;
                ++g.gateExits;
                edvr::setExternalCameraOnFoot(false);
                Log::get().note("head offset OFF: neither the panel nor a drawn "
                                "scene for %u frames, so the latch is being "
                                "dropped rather than guessed.", g.gateIdleFrames);
            }
        } else {
            g.gateIdleFrames = 0;
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
        if (g.gateExternal && (g.gateSinceEnter % 600) == 0 &&
            g.gateSinceEnter > 0) {
            Log::get().note("head offset still on: %u frames in, %u draws into "
                            "the eyes, %u frames since the panel.",
                            g.gateSinceEnter, eyeDraws,
                            g.gateSincePanel);
        }
        // Say when a candidate was REJECTED, and by which number.
        //
        // Without this a gate that never opens is indistinguishable from a
        // gate that is never asked, which is the whole difficulty of this
        // feature: entering the external camera, boarding the ship and
        // leaving Cinema Mode all look like "the panel stopped and a scene
        // appeared", and only the numbers separate them. These lines are
        // what a real discriminator would be built from.
        // EVERY condition, every time it declines, for the first 20.
        //
        // This used to fire only when no key was bound, which suppressed it
        // in exactly the configuration being debugged -- so the last flight
        // produced a gate that did not arm and NOTHING saying which test
        // failed. Three sessions have now been spent inferring the answer
        // from notes that do not carry the numbers.
        //
        // Printing all four values costs one line per rejection and removes
        // the guessing entirely: whichever one reads false is the bug.
        // Only once the panel has been seen enough to matter. The
        // budget was spent at frame 3062 last time -- during startup,
        // with panelRun=0 and no key yet pressed -- so by the moment
        // being debugged there were no lines left.
        if (!g.gateInCamera && sceneNow && g.gatePanelRun > 30 &&
            g.gateNearMisses < 20) {
            ++g.gateNearMisses;
            Log::get().note(
                "head offset NOT armed: panelRun=%u (needs >30), sincePanel=%u "
                "(needs >=8, window %u), eyeDraws=%u (needs >50), intent=%s, "
                "key %s, pressed %s, view=%d (wants %d).",
                g.gatePanelRun, g.gateSincePanel, g.gateEnterWindow, eyeDraws,
                g.gateIntent ? "set" : "CLEAR",
                g.gateKeyBound ? "BOUND" : "NOT BOUND -- nothing can arm",
                g.gateHaveKey ? "yes" : "not yet this session",
                g.gateViewIndex, g.gateWantView);
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
        // 90 frames is still generous for a stutter and far short of a mode
        // change anybody could act on.
        if (g.gateSincePanel > 90) g.gatePanelRun = 0;
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
        if (g.gateInCamera || !g.gateViewEverRead) {
            if (!g.gateViewSynced) {
                g.gateViewSynced = true;
                Log::get().note("camera view: the game says %d, the keypress count "
                                "said %d. Using the game's from here, so a missed "
                                "press no longer desyncs anything.",
                                gameView, g.gateViewIndex);
            }
            g.gateViewIndex = gameView;
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
            "  fix.head_offset_view = -1 applies the offset in every camera "
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
                            "is for view %d (fix.head_offset_view). Still in "
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
        if (g.gateIntentAge > g.gateIntentGrace) {
            g.gateIntent = false;
            Log::get().note("external camera intent expired: %u frames since the "
                            "key with the gate never arming, so that press was "
                            "not an entry into the camera. Cleared, so the next "
                            "press is a fresh one rather than a toggle back.",
                            g.gateIntentAge);
            g.gateIntentAge = 0;
        }
    }
}

}  // namespace edvr
